"""karst_render.py -- the picture Phase 0 is judged on.

Top-down conduit skeleton over hillshaded topography, plus vertical
cross-sections with the water table and the surface drawn, plus a statistics
panel. The owner judges pictures, not my readings of pictures
(`voxelsim-owner-judges-screenshots`), and an underground feature cannot be
photographed in-engine until the streaming and lighting work of Phase 3 lands --
so at Phase 0 a map and a long profile ARE the honest instrument.

Colour carries meaning, not decoration:
  * conduits are coloured by depth below the surface, so a multi-level system
    reads as multi-level rather than as a tangle;
  * sinks and springs are drawn as distinct marks, because "where does the water
    go in and come out" is the claim the whole method rests on;
  * the water table is drawn on every cross-section, because phreatic-vs-vadose
    is decided against it and a reader has to be able to check that.

Usage:
    python tools/karst_render.py <fields.npz> <network.npz> [--out DIR]
"""

from __future__ import annotations

import argparse
import pathlib

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt          # noqa: E402
import numpy as np                        # noqa: E402
from matplotlib.collections import LineCollection  # noqa: E402

GRID_M = 30.0


def hillshade(z, azimuth_deg=315.0, altitude_deg=45.0, vert_exag=2.0):
    gy, gx = np.gradient(z * vert_exag, GRID_M)
    slope = np.pi / 2.0 - np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(-gx, gy)
    az = np.radians(360.0 - azimuth_deg + 90.0)
    alt = np.radians(altitude_deg)
    sh = (np.sin(alt) * np.sin(slope)
          + np.cos(alt) * np.cos(slope) * np.cos(az - aspect))
    return np.clip(sh, 0, 1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fields", type=pathlib.Path)
    ap.add_argument("network", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path, default=None)
    ap.add_argument("--sections", type=int, default=3)
    args = ap.parse_args()
    out = args.out or args.fields.parent
    stem = args.fields.stem.replace("-fields", "")

    f = {k: v for k, v in np.load(args.fields).items()}
    net = dict(np.load(args.network).items())
    seg = net["segments"]                        # (n, 2, 3) in metres
    elev, head = f["elev_m"], f["head_m"]
    # Prefer the network stage's OWN springs/sinks: the field stage's are raw
    # candidates before suppression, and drawing those over a routed network
    # misrepresents the run. Fall back only for older files, and say so.
    springs = net.get("springs_used", f["springs"])
    sinks = net.get("sinks_used", f["sinks"])
    if "springs_used" not in net:
        print("  NOTE: network file predates springs_used; drawing RAW candidates")
    n, m = elev.shape
    extent = [0, m * GRID_M / 1000.0, n * GRID_M / 1000.0, 0]   # km, y down

    # ---------------- map ----------------
    fig, ax = plt.subplots(figsize=(11, 11), dpi=130)
    ax.imshow(hillshade(elev), cmap="gray", extent=extent, vmin=0, vmax=1)
    ax.imshow(elev, cmap="terrain", extent=extent, alpha=0.35)

    if len(seg):
        a, b = seg[:, 0, :], seg[:, 1, :]
        mx = np.clip((0.5 * (a[:, 0] + b[:, 0]) / GRID_M).astype(int), 0, m - 1)
        my = np.clip((0.5 * (a[:, 1] + b[:, 1]) / GRID_M).astype(int), 0, n - 1)
        depth = elev[my, mx] - 0.5 * (a[:, 2] + b[:, 2])
        lines = np.stack([a[:, :2] / 1000.0, b[:, :2] / 1000.0], axis=1)
        lc = LineCollection(lines, cmap="magma_r", linewidths=1.8,
                            norm=plt.Normalize(0, max(50.0, float(np.percentile(depth, 95)))))
        lc.set_array(depth)
        ax.add_collection(lc)
        cb = fig.colorbar(lc, ax=ax, fraction=0.035, pad=0.02)
        cb.set_label("conduit depth below surface (m)")

    if len(sinks):
        ax.scatter(sinks[:, 0] * GRID_M / 1000.0, sinks[:, 1] * GRID_M / 1000.0,
                   s=7, c="#1f77ff", marker="v", linewidths=0, label="sink (ponor)")
    if len(springs):
        ax.scatter(springs[:, 0] * GRID_M / 1000.0, springs[:, 1] * GRID_M / 1000.0,
                   s=26, facecolors="none", edgecolors="#00d0a0", linewidths=1.4,
                   label="spring (resurgence)")
    ax.set_xlabel("km"); ax.set_ylabel("km")
    ax.set_title(f"{stem}: karst conduit skeleton over topography\n"
                 f"{len(seg)} segments, {len(sinks)} sinks, {len(springs)} springs")
    ax.legend(loc="upper right", framealpha=0.9)
    fig.tight_layout()
    fig.savefig(out / f"{stem}-karst-map.png")
    plt.close(fig)

    # ---------------- cross-sections ----------------
    # Rows chosen where the network is DENSEST, not evenly spaced: an evenly
    # spaced section through a sparse row shows nothing and reads as "the model
    # produced nothing", which is a claim about the sampling, not the model.
    if len(seg):
        rows = np.clip((0.5 * (seg[:, 0, 1] + seg[:, 1, 1]) / GRID_M).astype(int), 0, n - 1)
        hist = np.bincount(rows, minlength=n)
        band = np.convolve(hist, np.ones(9), mode="same")
        picks = []
        for _ in range(args.sections):
            j = int(np.argmax(band))
            picks.append(j)
            band[max(0, j - 20):j + 20] = 0
    else:
        picks = list(np.linspace(n * 0.25, n * 0.75, args.sections).astype(int))

    fig, axes = plt.subplots(len(picks), 1, figsize=(13, 3.2 * len(picks)), dpi=130,
                             squeeze=False)
    xs_km = np.arange(m) * GRID_M / 1000.0
    for ax, j in zip(axes[:, 0], picks):
        ax.fill_between(xs_km, elev[j], elev[j].min() - 200, color="#c9bfae", lw=0)
        ax.plot(xs_km, elev[j], color="#4a3f33", lw=1.2, label="surface")
        ax.plot(xs_km, head[j], color="#2a7fff", lw=1.0, ls="--", label="water table")
        if len(seg):
            sel = np.abs(0.5 * (seg[:, 0, 1] + seg[:, 1, 1]) / GRID_M - j) <= 4
            if sel.any():
                s2 = seg[sel]
                lines = np.stack([np.c_[s2[:, 0, 0] / 1000.0, s2[:, 0, 2]],
                                  np.c_[s2[:, 1, 0] / 1000.0, s2[:, 1, 2]]], axis=1)
                ax.add_collection(LineCollection(lines, colors="#d1004f", linewidths=2.0))
        ax.set_ylabel("m")
        ax.set_title(f"section at y = {j * GRID_M / 1000.0:.2f} km", fontsize=9)
        ax.legend(loc="upper right", fontsize=8, framealpha=0.9)
        ax.set_xlim(xs_km[0], xs_km[-1])
    axes[-1, 0].set_xlabel("km")
    fig.tight_layout()
    fig.savefig(out / f"{stem}-karst-sections.png")
    plt.close(fig)

    print(f"wrote {out}/{stem}-karst-map.png and -karst-sections.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
