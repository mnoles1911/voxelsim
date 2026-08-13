"""Old crown model against new, same species, same seed, side by side.

The crown envelope changed under every tree, so the only honest way to show
what that did is to build both and put them next to each other. Same seed on
both sides: `pipeline.build` mixes the seed with a hash of the spec, so two
different specs never give the same individual, but they do give the same
individual-of-their-own-species, which is the closest thing to a controlled
comparison available here.

Old specs are read from the snapshot taken before the re-author, so this stops
working once that snapshot is gone -- which is fine, it is a one-off.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
from forge import pipeline, render, spec as sm

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT / ".backup" / "specs-20260810"
NEW = ROOT / "specs"
CELL = 330
SEED = 4


def main():
    names = sys.argv[2:]
    out = ROOT / "out" / sys.argv[1]
    cols = []
    for n in names:
        pair = []
        for label, root in (("before", OLD), ("after", NEW)):
            path = root / f"{n}.json"
            if not path.exists():
                pair.append((label, None, None))
                continue
            s, _ = sm.load(path)
            a = pipeline.build(s, SEED)
            img = render.render(a.grid, target_px=CELL - 40, ao=True)
            # `target_px` sizes the projection, not the returned canvas, and a
            # tall tree comes back well over the cell. Fit it here or the tiles
            # overlap each other and the sheet is unreadable.
            img.thumbnail((CELL - 16, CELL - 30), Image.LANCZOS)
            pair.append((label, img, a.stats))
        cols.append((n, pair))

    sheet = Image.new("RGB", (len(cols) * CELL, 2 * CELL + 46), (236, 236, 240))
    d = ImageDraw.Draw(sheet)
    for i, (n, pair) in enumerate(cols):
        cx = i * CELL
        for row, (label, img, st) in enumerate(pair):
            cy = row * CELL + 22
            if img is None:
                d.text((cx + 8, cy + 8), f"{label}: missing", (140, 40, 40))
                continue
            sheet.paste(img, (cx + (CELL - img.width) // 2,
                              cy + (CELL - img.height) // 2), img)
            d.text((cx + 8, cy + CELL - 14),
                   f"{label}  {st['height_m']:.1f} m  {st['voxels']:,} vox",
                   (40, 40, 46))
        d.text((cx + 8, 4), n, (10, 10, 14))
    out.parent.mkdir(exist_ok=True)
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()
