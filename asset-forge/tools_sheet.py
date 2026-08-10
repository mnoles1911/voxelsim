"""Contact sheet of one seed per named spec, for eyeballing shape."""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

from forge import pipeline, render, spec as sm

ROOT = Path(__file__).resolve().parent
SPECS = ROOT / "specs"
CELL = 300


def main():
    names = sys.argv[2:]
    out = ROOT / "out" / sys.argv[1]
    tiles = []
    for n in names:
        s, _ = sm.load(SPECS / f"{n}.json")
        a = pipeline.build(s, 1)
        img = render.render(a.grid, target_px=CELL - 30, ao=True)
        tiles.append((n, img, a.stats))

    cols = min(5, len(tiles))
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * CELL, rows * (CELL + 22)), (24, 25, 28))
    d = ImageDraw.Draw(sheet)
    for i, (n, img, st) in enumerate(tiles):
        cx, cy = (i % cols) * CELL, (i // cols) * (CELL + 22)
        sheet.paste(img, (cx + (CELL - img.width) // 2,
                          cy + (CELL - img.height) // 2), img)
        d.text((cx + 8, cy + CELL + 4),
               f"{n}  {st['height_m']:.1f} m  {st['voxels']:,} vox", (200, 202, 208))
    out.parent.mkdir(exist_ok=True)
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()
