"""Render a spec with every solid voxel forced to one bright material.

Separates SHAPE from COLOUR. Rock and bedrock preview colours are dark enough
that on the dark sheet background only the lit top faces show, which reads as a
flat wedge whatever the silhouette actually is.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
from forge import materials, pipeline, render, spec as sm

ROOT = Path(__file__).resolve().parents[1]
CELL = 320


def main():
    out = ROOT / "out" / sys.argv[1]
    names = sys.argv[2:]
    tiles = []
    for n in names:
        s, _ = sm.load(ROOT / "specs" / f"{n}.json")
        a = pipeline.build(s, 1)
        a.grid.data[a.grid.data != 0] = materials.MAT_SAND
        tiles.append((n, render.render(a.grid, target_px=CELL - 30, ao=True), a.stats))

    sheet = Image.new("RGB", (len(tiles) * CELL, CELL + 22), (200, 200, 205))
    d = ImageDraw.Draw(sheet)
    for i, (n, img, st) in enumerate(tiles):
        cx = i * CELL
        sheet.paste(img, (cx + (CELL - img.width) // 2, (CELL - img.height) // 2), img)
        d.text((cx + 8, CELL + 4), f"{n}  {st['height_m']:.1f} m", (20, 20, 24))
    out.parent.mkdir(exist_ok=True)
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()
