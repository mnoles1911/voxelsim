"""The LOD ladder, drawn at the size each level is actually seen at.

    python tools/lodsheet.py                       # the default six
    python tools/lodsheet.py red-fox grey-wolf     # pick your own

Each row is one species. The leftmost cell is the baked asset; each cell to the
right is a coarser reduction, and **every cell is drawn at the pixel size that
level is justified out to** rather than at a common scale. That is the whole
point: a LOD table in voxel counts tells you what you saved, and tells you
nothing about whether the animal still reads. Drawn at its own distance, a level
that has lost its legs is obvious and a level that has not is equally obvious.

THE THRESHOLDS ARE OPTICS, NOT TASTE. One pixel subtends 1.1312 mrad at 1080p
and a 70 degree vertical field of view, so a voxel of size v is exactly one pixel
at v / 1.1312 mrad -- 17.7 m for 2 cm, 88 m for 10 cm, 442 m for 50 cm. Beyond
that distance the finer grid is detail nobody can receive.
`docs/wildlife-lod-and-rings.md` is the argument; this is the picture.

WHAT TO LOOK FOR, since the owner judges these and I do not: whether the legs
survive, whether the head is still distinguishable from the body, and whether the
animal reads as its own species rather than as a generic quadruped blob. Those
three fail in that order as the lattice coarsens.
"""
import math
import sys

from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
from forge import lod, pipeline, render, spec as sm

ROOT = _path.ROOT
OUT = ROOT / "out" / "lod-ladder.png"

PX_RAD = math.radians(70) / 1080.0      # one pixel, in radians
LEVELS = (1, 2, 5, 10, 25)              # reduction factors; 1 is the baked grid
PAD = 14
LABEL_W = 150
SEED = 1

DEFAULT = ["white-tailed-deer", "grey-wolf", "red-fox", "american-bison",
           "european-rabbit", "brown-bear"]


def main():
    names = sys.argv[1:] or DEFAULT
    rows = []
    for name in names:
        spec, _ = sm.load(ROOT / "specs" / f"{name}.json")
        # Variation off: the ladder is about the lattice, and a 9% length jitter
        # between cells would be read as the reduction doing something.
        spec, _ = sm.patch(spec, {"variation.amount": 0.0})
        asset = pipeline.build(spec, SEED)
        base_mm = int(round(asset.grid.voxel_m * 1000.0))
        cells = []
        for f in LEVELS:
            data = (asset.grid.data if f == 1
                    else lod.reduce_to(asset.grid.data, f))
            n = int((data != 0).sum())
            vox_mm = base_mm * f
            # The range this level is justified out to, and therefore the size
            # it should be drawn at: one voxel = one pixel.
            reach_m = (vox_mm / 1000.0) / PX_RAD
            metres = max(data.shape) * (vox_mm / 1000.0)
            px = max(4, int(round(metres / (reach_m * PX_RAD))))
            if n < lod.MIN_VOXELS and f > 1:
                cells.append((f, vox_mm, reach_m, n, None))
                continue
            g = type(asset.grid)(data.shape, lod.reduce_origin(asset.grid.origin, f)
                                 if f > 1 else asset.grid.origin, vox_mm / 1000.0)
            g.data[:] = data
            cells.append((f, vox_mm, reach_m, n, render.render(g, target_px=px,
                                                               ao=True)))
        rows.append((name, cells))

    cell_w = max(max((c[4].width if c[4] else 8) for c in cs) for _, cs in rows) + PAD * 2
    cell_w = max(cell_w, 150)
    row_h = [max((c[4].height if c[4] else 8) for c in cs) + 40 for _, cs in rows]
    sheet = Image.new("RGB", (LABEL_W + len(LEVELS) * cell_w, sum(row_h)),
                      (24, 25, 28))
    d = ImageDraw.Draw(sheet)

    y = 0
    for r, (name, cells) in enumerate(rows):
        h = row_h[r]
        d.text((8, y + h // 2 - 6), name, (235, 238, 244))
        for c, (f, vox_mm, reach_m, n, img) in enumerate(cells):
            x = LABEL_W + c * cell_w
            if img is not None:
                sheet.paste(img, (x + (cell_w - img.width) // 2,
                                  y + (h - 34 - img.height)), img)
            else:
                d.text((x + 10, y + h // 2), "below the floor\n(under "
                       f"{lod.MIN_VOXELS} voxels)", (220, 120, 120))
            d.text((x + 8, y + h - 30),
                   f"{vox_mm / 10:g} cm   {n:,} vox", (198, 202, 210))
            d.text((x + 8, y + h - 17),
                   f"drawn as at {reach_m:,.0f} m", (150, 154, 162))
        y += h

    OUT.parent.mkdir(exist_ok=True)
    sheet.save(OUT)
    print(f"{OUT}   {sheet.width}x{sheet.height}")
    print("each cell drawn at the size that level is justified out to")


if __name__ == "__main__":
    main()
