"""Calibrate `BakeConstants.stream_K` against CORRECTLY ROUTED drainage.

`stream_K = 0.15` is marked UNCALIBRATED in `bake/pipeline.py`, and it is: it was
chosen on a field whose priority-flood was a plain fill, so every filled pit was
a level lake, MFD terminated there, and 69.2% of the land never reached the sea.
With Barnes' epsilon fill the catchment areas grow ~2.9x, and incision goes as
A^m -- so the old K over-carves by roughly 1.6x on trunk channels.

The plan is explicit that no summary statistic in it ever distinguished a good K
from a bad one, and that a hillshade is what does. So this reports BOTH: the
incision statistics that let you compare candidates numerically, and a hillshade
strip so the choice is made by eye on real terrain rather than by a table.

Run with the terrain-diffusion venv python (needs numba/scipy):
  calibrate_stream_k.py <tile.vxtl> [--scale 16] [--size 2048] [--png out.png]

`--size` crops the fine domain rather than baking a full 8192^2 + apron, whose
live set is ~3 GB; the crop keeps K comparison honest because every candidate
sees the identical routed field -- only K changes between arms.
"""
import argparse, struct, sys, time
import numpy as np

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[1]))

from terrain_service.bake import fill_depressions, d8_receivers, accumulate_mfd
from terrain_service.bake.noise import carrier, roughness
from terrain_service.bake.incise import stream_power
from terrain_service.bake.thermal import relax

TILE = 512


def decode_vxtl(path):
    b = open(path, "rb").read()
    magic, _v, _s, _x, _y, _sc, size = struct.unpack_from("<4sHQiiBH", b, 0)
    assert magic == b"VXTL"
    off = struct.calcsize("<4sHQiiBH")
    return np.frombuffer(b, dtype="<i2", count=size * size,
                         offset=off).reshape(size, size).astype(np.float32)


def hillshade(z, cell_m, az=315.0, alt=45.0):
    gy, gx = np.gradient(z.astype(np.float64), cell_m)
    slope = np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(-gx, gy)
    a, t = np.radians(az), np.radians(alt)
    return np.clip(np.sin(t) * np.cos(slope) +
                   np.cos(t) * np.sin(slope) * np.cos(a - aspect), 0, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tile")
    ap.add_argument("--scale", type=int, default=16)
    ap.add_argument("--size", type=int, default=2048, help="fine crop edge")
    ap.add_argument("--iters", type=int, default=48)
    ap.add_argument("--png")
    ap.add_argument("--ks", default="0.04,0.06,0.09,0.15")
    ap.add_argument("--a-crit", type=float, default=None,
                    help="channel-initiation area in m2; 0 disables the gate and "
                         "reproduces the pre-2026-07-29 behaviour")
    a = ap.parse_args()

    cell_m = 30.0 / a.scale
    coarse_edge = a.size // a.scale
    coarse = decode_vxtl(a.tile)
    # Bake a crop, not the whole tile: every K arm must see the SAME routed
    # field, and 3 GB per arm is not the way to get that.
    c0 = (TILE - coarse_edge) // 2
    coarse = np.ascontiguousarray(coarse[c0:c0 + coarse_edge, c0:c0 + coarse_edge])
    print(f"tile {a.tile}")
    print(f"  coarse crop {coarse.shape} -> fine {a.size}^2 @ {cell_m} m/px "
          f"({a.size*cell_m/1000:.2f} km)")

    t0 = time.process_time()
    z0 = carrier(coarse, a.scale)
    gy, gx = np.gradient(z0, cell_m)
    slope0 = np.hypot(gx, gy)
    z0 = z0 + roughness(z0, cell_m, slope0, seed=20260729)
    print(f"  B0+B1                       {time.process_time()-t0:6.2f} s cpu")

    t0 = time.process_time()
    filled = fill_depressions(z0)
    rec, d8slope = d8_receivers(filled, cell_m)
    acc = accumulate_mfd(filled, cell_m)
    print(f"  B2 fill+D8+MFD              {time.process_time()-t0:6.2f} s cpu")

    interior_sinks = int((rec[1:-1, 1:-1] == -1).sum())
    print(f"\n  ROUTING CHECK  interior dead-ends {interior_sinks} "
          f"(must be 0; the prototype had 341,368)")
    print(f"  max catchment {acc.max()/1e6:.2f} km2   "
          f"cells > 0.1 km2 {(acc > 1e5).sum()}")

    ks = [float(x) for x in a.ks.split(",")]
    results = {}

    # Incision depth alone cannot separate "carved correctly" from "over-carved":
    # both go up with K, monotonically and without a kink. The slope distribution
    # can. Real landscapes are slope-limited -- hillsides sit near the angle of
    # repose and essentially nothing exceeds it, because anything steeper fails --
    # so a fat tail above repose is the signature of a K that is cutting gorges
    # the thermal pass cannot repair. That, plus the hillshade, is what decides.
    repose_tan = np.tan(np.radians(36.0))
    gy, gx = np.gradient(filled.astype(np.float64), cell_m)
    base_over = float((np.hypot(gx, gy) > repose_tan).mean())
    print(f"\n  pre-incision fraction over 36 deg repose: {base_over*100:.2f} %")

    print(f"\n  {'K':>7} {'p50':>7} {'p99':>7} {'max':>7} | {'>repose':>8} {'p99 slp':>8} "
          f"| {'chan %':>7} {'relief':>7}")
    print(f"  {'':>7} {'':>7} {'incision m':>7} {'':>7} | {'':>8} {'deg':>8} "
          f"| {'':>7} {'m':>7}")
    for K in ks:
        kw = {} if a.a_crit is None else {"a_crit_m2": a.a_crit}
        inc = stream_power(acc, d8slope, K=K, cap_m=25.0, **kw)
        z = relax(filled - inc, cell_m, iters=a.iters, rate=0.4)
        results[K] = z
        gy, gx = np.gradient(z.astype(np.float64), cell_m)
        s = np.hypot(gx, gy)
        over = (s > repose_tan).mean() * 100.0
        p99s = np.degrees(np.arctan(np.percentile(s, 99)))
        # "Channel" = incised by more than a voxel; drainage density's cheap proxy.
        chan = (inc > 0.1).mean() * 100.0
        print(f"  {K:>7.3f} {np.percentile(inc,50):7.3f} {np.percentile(inc,99):7.3f} "
              f"{inc.max():7.2f} | {over:7.2f}% {p99s:8.1f} | {chan:6.1f}% "
              f"{z.max()-z.min():7.1f}")

    print("\n  Read the table this way: incision columns rise monotonically with K and")
    print("  so cannot pick a winner on their own. The '>repose' column is the one")
    print("  with a floor to violate -- it should stay close to the pre-incision")
    print("  figure above. A K that inflates it is cutting walls steeper than rock")
    print("  stands, which reads in-engine as gorges, not valleys.")
    print("  The mis-routed prototype field gave p99 0.63 m at K=0.012 and 7.84 m at")
    print("  K=0.15, and K=0.15 was judged right by eye THERE -- on a field with two")
    print("  thirds of its drainage stranded, so that judgement does not transfer.")

    if a.png:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        # TWO rows, because they answer different questions and a single crop
        # answers neither. The full domain is where trunk valleys live -- a 1.4 km
        # window into a 7.7 km tile is nearly all hillslope, and judging dissection
        # from it says more about where the crop landed than about K. The zoom is
        # where voxel-scale artifacts live, which the full view averages away.
        c = min(768, a.size)
        o = (a.size - c) // 2
        zl = (slice(o, o + c), slice(o, o + c))
        n = len(ks) + 1
        fig, ax = plt.subplots(2, n, figsize=(5.2 * n, 10.8))
        ax[0][0].imshow(np.log10(acc + 1.0), cmap="magma", interpolation="nearest")
        ax[0][0].set_title(f"MFD accumulation, full {a.size*cell_m/1000:.1f} km",
                           fontsize=10)
        ax[1][0].imshow(np.log10(acc[zl] + 1.0), cmap="magma", interpolation="nearest")
        ax[1][0].set_title(f"zoom {c*cell_m/1000:.2f} km", fontsize=10)
        for i, K in enumerate(ks):
            ax[0][i + 1].imshow(hillshade(results[K], cell_m), cmap="gray",
                                vmin=0, vmax=1, interpolation="nearest")
            ax[0][i + 1].set_title(f"K = {K}   full domain", fontsize=10)
            ax[1][i + 1].imshow(hillshade(results[K][zl], cell_m), cmap="gray",
                                vmin=0, vmax=1, interpolation="nearest")
            ax[1][i + 1].set_title(f"K = {K}   zoom", fontsize=10)
        for row in ax:
            for x in row:
                x.set_xticks([]); x.set_yticks([])
        fig.suptitle(f"stream_K on CORRECTLY ROUTED drainage - {a.size}^2 "
                     f"at {cell_m} m/px", fontsize=13)
        fig.tight_layout()
        fig.savefig(a.png, dpi=105)
        print(f"\n  wrote {a.png}")


if __name__ == "__main__":
    main()
