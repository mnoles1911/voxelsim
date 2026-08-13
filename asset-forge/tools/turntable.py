"""Render one asset from four sides.

The preview renderer looks down a fixed isometric corner, which is a bad angle
for anything whose defining feature is a hole. A 40 m arch with a third of its
silhouette open rendered as a solid lens from that one corner, and the natural
reading of that picture -- "the arch carve does nothing" -- was wrong. Four
quarter turns settle it in one sheet.

Rotating the grid rather than the camera keeps the renderer untouched, and
quarter turns are exact so nothing is resampled.

The four turns fix the AZIMUTH, but the camera's HEIGHT is the other half of
the same defect and no amount of turning fixes it: from 35 degrees up, a stack
of boulders has its gaps filled in by the boulders behind and a balanced rock
has its undercut hidden under its own cap. So rocks are drawn in side
elevation by default (`tools/elevation.py`); `--iso` restores the old camera
and `--both` draws both rows.

    python tools/turntable.py arch.png hero-natural-arch [--bright]
    python tools/turntable.py stack.png hero-tor-stack --both
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
import elevation as ev
from forge import materials, pipeline, render, spec as sm
from forge.grid import VoxelGrid

ROOT = Path(__file__).resolve().parents[1]
CELL = 400


def main():
    args, flags, opts = ev.split_args(sys.argv[1:])
    bright = "--bright" in flags
    out = ROOT / "out" / args[0]
    name = args[1]

    # A 40 m arch is three to five minutes to rebuild, and the questions worth
    # asking about one come in batches, so `tools/archprobe.py` leaves the built
    # grid behind and this will use it.
    cache = ROOT / "out" / f"{name}.npz"
    spec = None
    if "--cached" in flags and cache.exists():
        z = np.load(cache)
        a = type("A", (), {})()
        a.grid = VoxelGrid((1, 1, 1), voxel_m=float(z["voxel_m"]))
        a.grid.data = z["data"]
        a.stats = {"height_m": a.grid.data.shape[2] * a.grid.voxel_m}
    else:
        spec, _ = sm.load(ROOT / "specs" / f"{name}.json")
        a = pipeline.build(spec, opts["seed"])
    if bright:
        a.grid.data[a.grid.data != 0] = materials.MAT_SAND

    if spec is None:
        # A cached grid carries no spec, so the kind is unknown; a turntable is
        # only ever pointed at a rock, so default to the elevation there too.
        cameras = (["side", "iso"] if "--both" in flags else
                   ["iso"] if "--iso" in flags else ["side"])
    else:
        cameras = ev.cameras_for(flags, spec)

    # Which turn a one-still preview would choose, marked on the sheet so the
    # four-way and the gallery tile cannot disagree without it being obvious.
    best = render.best_turn(a.grid.data)

    rows = []
    for camera in cameras:
        turns = [ev.turned(a.grid, t) for t in range(4)]
        sc = ev.common_scale(turns, camera=camera, target_px=CELL - 30,
                             tilt_deg=opts["tilt"])
        imgs = ev.fit([ev.view(g, camera, scale=sc, target_px=CELL - 30,
                               tilt_deg=opts["tilt"], ao=True) for g in turns],
                      CELL - 16)
        rows.append([(f"{t * 90}°" + f" {camera}"
                      + ("   <- preview picks this" if t == best else ""), img)
                     for t, img in enumerate(imgs)])

    sheet = Image.new("RGB", (4 * CELL, len(rows) * (CELL + 26)), (200, 200, 205))
    d = ImageDraw.Draw(sheet)
    for r, tiles in enumerate(rows):
        cy = r * (CELL + 26)
        for i, (lbl, img) in enumerate(tiles):
            cx = i * CELL
            sheet.paste(img, (cx + (CELL - img.width) // 2,
                              cy + (CELL - img.height) // 2), img)
            d.text((cx + 8, cy + CELL + 6), lbl, (20, 20, 24))
    st = a.stats
    d.text((4 * CELL - 210, (len(rows) - 1) * (CELL + 26) + CELL + 6),
           f"{name}  {st['height_m']:.1f} m tall", (20, 20, 24))
    out.parent.mkdir(exist_ok=True)
    sheet.save(out)
    print(f"{out}  height {st['height_m']:.1f} m")


if __name__ == "__main__":
    main()
