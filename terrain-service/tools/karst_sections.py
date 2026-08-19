"""karst_sections.py -- what these caves actually look like, drawn at voxel scale.

TWO SHEETS, because they answer two different questions.

SHEET 1 -- PASSAGE CROSS-SECTIONS. What a player standing in each passage type
sees, drawn at the engine's own 10 cm voxel with the player's 0.6 x 1.8 m box to
scale. The types are the real karst vocabulary and each one is a different
answer to "where was the water table when this formed":

    crawl            small phreatic tube, the tight connector
    phreatic tube    formed full of water: circular, because pressure dissolves
                     evenly in all directions
    vadose canyon    formed by a free-surface stream cutting DOWN: tall, narrow,
                     and the reason caves have slot passages
    keyhole          a phreatic tube the water table later dropped out of, so a
                     vadose notch is incised into its floor. The most
                     recognisable cave cross-section there is.
    chamber          breakdown-widened, flat-floored with a rubble cone
    shaft            vertical, formed on a joint

SHEET 2 -- HOW THEY BREAK SURFACE. Profile schematics of the four ways a conduit
meets daylight, which is the half the previous system got most obviously wrong
(a naked vertical cylinder, rejected twice).

    doline           collapse/solution hollow, funnel-shaped, conduit at the base
    hillside mouth   a passage intersected by a valley wall -- the classic
                     walk-in entrance, and the one a player wants
    swallet          a surface stream sinking into its own bed
    resurgence       a spring: the conduit daylights at the water table

EVERY SHAPE HERE IS DRAWN BY THE SAME CODE THE VOXELISER USES -- capsule SDF,
sediment floor, wall roughness -- rather than being illustrated by hand. A
schematic that does not share its geometry with the generator is a drawing of
what someone hoped the generator would do.

Usage:
    python tools/karst_sections.py [--out DIR] [--vox-m 0.1]
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt              # noqa: E402
import numpy as np                            # noqa: E402
from matplotlib.patches import Rectangle      # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from karst_voxel import _h3                   # noqa: E402  -- one noise, not two

PLAYER_W, PLAYER_H, PLAYER_CROUCH = 0.60, 1.80, 1.20

# name, radius m, vertical stretch, sediment fill (fraction of diameter),
# keyhole notch (depth m, half-width m) or None, roughness
TYPES = [
    ("crawl",         0.8, 1.00, 0.30, None,        0.16),
    ("phreatic tube", 2.2, 1.00, 0.28, None,        0.14),
    ("vadose canyon", 1.4, 3.20, 0.18, None,        0.20),
    ("keyhole",       2.4, 1.00, 0.10, (3.0, 0.55), 0.15),
    ("chamber",       7.0, 0.62, 0.34, None,        0.22),
    ("shaft",         1.6, 1.00, 0.00, None,        0.18),   # round in section; see the profile sheet
]


def rough(xx, zz, amp, seed):
    """Multi-octave wall perturbation, the same idea the cavern pass ships
    (caverns.h's kCavernRoughAmpQ10) rather than a new one."""
    out = np.zeros_like(xx)
    for i, (cell, a) in enumerate(((3.0, 1.0), (1.1, 0.5), (0.45, 0.25))):
        gx = np.floor(xx / cell).astype(np.int64)
        gz = np.floor(zz / cell).astype(np.int64)
        out += a * _h3(gx, gz, np.int64(i * 977), seed)
    return out * amp


def section(name, r, vstretch, sed, notch, ruf, vox, half):
    """One passage cross-section as a boolean air mask."""
    n = int(2 * half / vox)
    ax = (np.arange(n) - n / 2) * vox
    X, Z = np.meshgrid(ax, ax, indexing="ij")
    d = np.sqrt(X ** 2 + (Z / vstretch) ** 2) - r
    d = d + rough(X, Z, ruf * r, 4242)
    air = d <= 0
    # SEDIMENT FIRST, THEN THE NOTCH. Applying the flat fill last erased the
    # keyhole's vadose slot entirely -- the slot lives BELOW the tube floor,
    # which is exactly what the sediment mask cuts away. A keyhole with its
    # notch filled in is just a tube, i.e. the one cross-section everybody
    # recognises, rendered as the one that carries no information.
    if sed > 0:
        floor = -r * vstretch + 2.0 * r * vstretch * sed
        air &= Z >= floor
    if notch is not None:                      # keyhole: vadose slot in the floor
        depth, hw = notch
        slot = (np.abs(X) < hw + rough(X, Z, 0.10, 99)) & (Z < 0) & (Z > -r - depth)
        air |= slot
    return ax, air


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=pathlib.Path,
                    default=pathlib.Path("D:/voxelsim/bake-out/karst"))
    ap.add_argument("--vox-m", type=float, default=0.1)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    v = args.vox_m

    # ---------------- SHEET 1: cross-sections --------------------------------
    fig, axes = plt.subplots(1, len(TYPES), figsize=(3.1 * len(TYPES), 4.6), dpi=140)
    for ax, (name, r, vs, sed, notch, ruf) in zip(axes, TYPES):
        half = max(4.0, r * vs * 1.5, (r * vs + (notch[0] if notch else 0)) * 1.25)
        xs, air = section(name, r, vs, sed, notch, ruf, v, half)
        ax.imshow(air.T, origin="lower", cmap="bone",
                  extent=[xs[0], xs[-1], xs[0], xs[-1]], interpolation="nearest")
        # player to scale, standing on the floor
        col = np.argmin(np.abs(xs))
        colair = np.nonzero(air[col])[0]
        fz = xs[colair[0]] if len(colair) else -r
        crouch = (xs[colair[-1]] - fz) < PLAYER_H if len(colair) else True
        ax.add_patch(Rectangle((-PLAYER_W / 2, fz), PLAYER_W,
                               PLAYER_CROUCH if crouch else PLAYER_H,
                               fill=False, ec="#d1004f", lw=1.8))
        ax.set_title(f"{name}\nr={r:.1f} m" + ("  (crouch)" if crouch else ""), fontsize=10)
        ax.set_xlabel("m"); ax.set_aspect("equal")
    axes[0].set_ylabel("m")
    fig.suptitle(f"passage cross-sections at {v*100:.0f} cm voxels — "
                 f"player box 0.6 x 1.8 m drawn to scale (red)", fontsize=12)
    fig.tight_layout()
    fig.savefig(args.out / "karst-cross-sections.png")
    plt.close(fig)

    # ---------------- SHEET 2: how they break surface ------------------------
    W, H, vv = 60.0, 40.0, 0.15
    nx, nz = int(W / vv), int(H / vv)
    xs = np.linspace(-W / 2, W / 2, nx)
    zs = np.linspace(-H * 0.72, H * 0.28, nz)
    X, Z = np.meshgrid(xs, zs, indexing="ij")

    def draw(ax, surf, conduits, title, note):
        air = Z > surf[:, None]                       # above ground
        for (a, b, r) in conduits:
            a, b = np.asarray(a, float), np.asarray(b, float)
            ab = b - a
            t = np.clip(((X - a[0]) * ab[0] + (Z - a[1]) * ab[1]) / max(ab @ ab, 1e-9), 0, 1)
            d = np.sqrt((X - a[0] - ab[0] * t) ** 2 + (Z - a[1] - ab[1] * t) ** 2)
            air |= (d - r + rough(X, Z, 0.16 * r, 7)) <= 0
        # ROCK DARK, AIR LIGHT. The first version drew air black and rock
        # white, which is internally consistent and reads backwards against
        # every cave survey ever published -- the reader sees the void as the
        # solid. `bone` rather than `bone_r`.
        ax.imshow(air.T, origin="lower", cmap="bone",
                  extent=[xs[0], xs[-1], zs[0], zs[-1]], interpolation="nearest")
        ax.plot(xs, surf, color="#2a7f2a", lw=1.6)
        ax.set_title(title, fontsize=11)
        ax.text(0.02, 0.03, note, transform=ax.transAxes, fontsize=8,
                va="bottom", ha="left", color="#e8e8e8")
        ax.set_xlabel("m"); ax.set_ylabel("m"); ax.set_aspect("equal")

    fig, axes = plt.subplots(2, 2, figsize=(15, 11), dpi=140)

    # doline: a solution hollow, conduit rising into its base
    surf = 6.0 - 9.0 * np.exp(-(xs / 7.0) ** 2)
    draw(axes[0][0], surf,
         [((0, -2.0), (0, -16.0), 2.0), ((0, -16.0), (24, -20.0), 2.4)],
         "doline — solution hollow over a conduit",
         "the funnel is the SURFACE dissolving, not a drilled shaft;\n"
         "the conduit rises to meet it from below")

    # hillside mouth: passage cut by a valley wall
    surf = np.clip(14.0 - 0.62 * (xs + 30.0), -14.0, 14.0)
    draw(axes[0][1], surf,
         [((-26, -6.0), (26, -12.0), 2.6)],
         "hillside mouth — a passage cut open by the valley",
         "the walk-in entrance: the valley wall retreats\n"
         "until it intersects an existing conduit")

    # swallet: stream sinking into its own bed
    surf = 2.0 + 1.2 * np.sin(xs / 9.0) - 6.0 * np.exp(-((xs - 4) / 4.0) ** 2)
    draw(axes[1][0], surf,
         [((4, -4.0), (10, -13.0), 1.5), ((10, -13.0), (28, -17.0), 1.8)],
         "swallet — a surface stream sinking into its bed",
         "where the bake's own channel crosses an inception horizon;\n"
         "this is the sink the router starts from")

    # resurgence: spring at the water table in a valley floor
    surf = np.clip(-4.0 + 0.5 * np.abs(xs + 10.0), -6.0, 16.0)
    # THE CONDUIT MUST REACH THE SURFACE. The first version stopped it 2 m short
    # of the valley floor, so the "spring" was a sealed pocket -- the one thing a
    # resurgence cannot be. Its last segment now rises to the terrain line.
    draw(axes[1][1], surf,
         [((-30, -9.0), (-14, -7.0), 2.2), ((-14, -7.0), (-10.0, -4.2), 1.8)],
         "resurgence — the spring the conduits drain to",
         "conduit daylights where the water table meets the valley floor;\n"
         "this is the router's destination")

    fig.suptitle("how conduits break surface — profile schematics, 15 cm voxels, "
                 "green line is the terrain", fontsize=13)
    fig.tight_layout()
    fig.savefig(args.out / "karst-entrances.png")
    plt.close(fig)

    print(f"wrote {args.out}/karst-cross-sections.png and karst-entrances.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
