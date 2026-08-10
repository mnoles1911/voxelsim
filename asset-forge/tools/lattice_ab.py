"""Author-for-2cm versus author-for-5cm, same ground, side by side.

The point is the word AUTHOR. Shrinking a 2 cm asset into a 5 cm lattice
answers a different and easier question than designing one for 5 cm, and
answering the easy question is how you end up rejecting a lattice that would
have been fine. Each row here is the same plant tuned twice, once for each
voxel size, and rendered at the same physical scale so the comparison is about
detail and not about size on screen.
"""
import sys

from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
from forge import pipeline, render, spec as sm

ROOT = _path.ROOT
CELL = 330

# Each entry: label, base spec name, overrides for 2 cm, overrides for 5 cm.
CASES = [
    ("grass", "meadow-grass",
     {"resolution_cm": "2"},
     # At 5 cm a blade is one voxel wide whatever you ask for, so 34 threads
     # would land on top of each other. Fewer, taller, wider-spread blades is
     # what the lattice can actually express.
     {"resolution_cm": "5", "height_m": 0.45, "tuft.stems": 13,
      "tuft.width_m": 0.05, "tuft.spread_m": 0.09, "tuft.base_m": 0.09,
      "tuft.arc": 0.6, "tuft.length_var": 0.4}),

    ("reed", "water-reed",
     {"resolution_cm": "2"},
     {"resolution_cm": "5", "tuft.stems": 12, "tuft.width_m": 0.055,
      "tuft.head_m": 0.09, "tuft.spread_m": 0.22, "tuft.base_m": 0.12}),

    ("flower (daisy-size head)", "meadow-daisy",
     {"resolution_cm": "1"},
     {"resolution_cm": "5", "height_m": 0.4, "tuft.stems": 7,
      "tuft.width_m": 0.05, "tuft.head_m": 0.12, "tuft.base_m": 0.07}),

    ("flower (big head)", "meadow-daisy",
     {"resolution_cm": "1", "tuft.head_m": 0.1, "height_m": 0.5},
     # How big does a bloom have to be before 5 cm can hold a rosette at all?
     {"resolution_cm": "5", "height_m": 0.7, "tuft.stems": 6,
      "tuft.width_m": 0.05, "tuft.head_m": 0.26, "tuft.base_m": 0.07}),
]


def main():
    out = ROOT / "out" / (sys.argv[1] if len(sys.argv) > 1 else "lattice-ab.png")
    rows = []
    for label, base, fine, coarse in CASES:
        s, _ = sm.load(ROOT / "specs" / f"{base}.json")
        built = []
        for tag, over in (("author 2 cm", fine), ("author 5 cm", coarse)):
            spec, _ = sm.patch(s, over)
            a = pipeline.build(spec, 3)
            built.append((f"{tag} @ {sm.get(spec, 'resolution_cm')} cm", a))
        rows.append((label, built))

    # One scale PER ROW: the two variants of a plant must be drawn at the same
    # physical scale as each other, but a 40 cm tuft and a 2 m reed should each
    # fill their own row. Scaling the whole sheet together shrank the tuft to
    # forty pixels and made the comparison meaningless.
    sheet = Image.new("RGB", (2 * CELL + 150, len(rows) * (CELL + 24)), (24, 25, 28))
    d = ImageDraw.Draw(sheet)
    for r, (label, built) in enumerate(rows):
        y = r * (CELL + 24)
        d.text((8, y + CELL // 2), label, (235, 238, 244))
        row_max = max(max(a.grid.shape) * a.grid.voxel_m for _, a in built)
        for c, (tag, a) in enumerate(built):
            metres = max(a.grid.shape) * a.grid.voxel_m
            px = max(40, int((CELL - 40) * metres / row_max))
            img = render.render(a.grid, target_px=px, ao=True)
            x = 150 + c * CELL
            sheet.paste(img, (x + (CELL - img.width) // 2,
                              y + (CELL - img.height) - 6), img)
            d.text((x + 8, y + CELL + 5),
                   f"{tag}   {a.stats['voxels']:,} vox   "
                   f"{a.stats['height_m']:.2f} m", (198, 202, 210))
    out.parent.mkdir(exist_ok=True)
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()
