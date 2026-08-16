"""The same animal authored at 5 cm and at 2 cm, at ONE pixels-per-metre.

WHY THIS SHEET EXISTS. `resolution_cm` is authored per species and the land
library disagrees with itself about it: measured 2026-08-15, of 131 quadrupeds
42 sit on 1 cm, 63 on 2 cm and 26 on 5 cm -- and the 26 on 5 cm are the LARGEST
animals, the ones a player sees first and longest. A lion is 2.0 m on 5 cm and
so is 40 voxels long; a tiger is 2.0 m on 2 cm and is 100. A horse is 42 voxels
and a zebra is 115. Nothing chose that. It is where three separate seeding
passes happened to land, and it is the reason `docs/quadruped-limb-regression.md`
had to report that 71 of 131 species are lattice-limited with 46 of them under
five voxels of visible limb: below about 60 voxels along the body there is no
value of `quad.leg_thick` that can draw a leg, because the leg is three voxels
wide by the floor and nothing else is available.

SO THE QUESTION IS NOT A SPEC ROW, IT IS A LATTICE, and it is the owner's.

ONE SCALE FOR THE WHOLE SHEET, IN PIXELS PER METRE, AND THAT IS THE WHOLE
POINT. The first lattice comparison this project ever drew let each image size
itself to its own grid, which drew the 5 cm oak BIGGER than the 2 cm one and
made the coarse lattice look like the generous choice. A comparison about
detail has to hold physical size fixed or it is a comparison about nothing.
Here a metre is `PX_PER_M` pixels in every cell of every row.

    python tools/quadlattice_ab.py                    # the default six
    python tools/quadlattice_ab.py lion muskox        # pick your own

Each cell prints the voxel count, because the cost is half the decision: the
species already shipping on 2 cm cost 27,000-50,000 voxels, and everything at or
under about 2.4 m lands in that same band when moved. The giants do not --
a 6 m elephant is 975,538 -- and they are a separate question.
"""
import sys

from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
from forge import pipeline, render, spec as sm

ROOT = _path.ROOT
OUT = ROOT / "out" / "quad-lattice-ab.png"

# Pixels per metre, held across the entire sheet. 150 puts a 2 m animal at
# 300 px, which is enough to see a leg fail to exist.
PX_PER_M = 150.0

PAD = 16
LABEL_W = 170
SEED = 1

# Species on 5 cm today, paired with a SHIPPED 2 cm animal of the same length
# wherever one exists -- because "the coarse one looks fine on its own" is the
# easy question, and the sheet should ask the hard one instead.
DEFAULT = [
    ("muskox", "blue-wildebeest"),        # 2.2 m both
    ("lion", "bengal-tiger"),             # 2.0 m both
    ("przewalskis-horse", "plains-zebra"),  # 2.1 m vs 2.3 m
    ("brown-bear", None),
    ("reindeer", "white-tailed-deer"),    # 1.9 m vs 1.8 m
    ("elk-wapiti", "red-deer-stag"),      # 2.4 m vs 2.0 m
]


def _build(name: str, cm: str | None):
    s, _ = sm.load(ROOT / "specs" / f"{name}.json")
    if cm is not None:
        s, _ = sm.patch(s, {"resolution_cm": cm})
    # Variation off: this sheet is about the lattice, and a 9% length jitter
    # between two cells would be read as the lattice doing something.
    s, _ = sm.patch(s, {"variation.amount": 0.0})
    got = sm.get(s, "resolution_cm")
    if cm is not None and float(got) != float(cm):
        # `resolution_cm` is a CHOICE and an out-of-menu value falls back to the
        # default without a word. Asking for 3 cm returns a 5 cm build with a
        # 5 cm voxel count and no complaint. Never let that reach a picture.
        raise SystemExit(f"{name}: asked {cm} cm, spec resolved to {got} cm -- "
                         f"out-of-menu value silently substituted")
    return pipeline.build(s, SEED), float(got)


def main():
    names = sys.argv[1:]
    cases = ([(n, None) for n in names] if names else DEFAULT)

    rows = []
    for name, control in cases:
        cells = []
        for label, cm in (("authored", None), ("at 2 cm", "2")):
            a, cm_got = _build(name, cm)
            cells.append((f"{name}  {label} ({cm_got:g} cm)", a))
        if control:
            a, cm_got = _build(control, None)
            cells.append((f"{control}  shipped ({cm_got:g} cm)", a))
        rows.append(cells)

    # Render everything first, then size the sheet to what was actually drawn.
    # `render` returns a tight image, so a cell sized to the largest animal in
    # the LIBRARY leaves a bull moose's worth of empty pixels around a fox and
    # shrinks the thing being judged. The scale stays global; only the boxes
    # around it are per-row.
    drawn = [[(tag, render.render(a.grid,
                                  target_px=max(8, int(max(a.grid.shape)
                                                       * a.grid.voxel_m
                                                       * PX_PER_M)), ao=True),
               a.stats["voxels"]) for tag, a in cells] for cells in rows]

    cell_w = max(im.width for r in drawn for _, im, _ in r) + PAD * 2
    row_h = [max(im.height for _, im, _ in r) + 26 for r in drawn]
    cols = max(len(r) for r in drawn)
    sheet = Image.new("RGB", (LABEL_W + cols * cell_w, sum(row_h)), (24, 25, 28))
    d = ImageDraw.Draw(sheet)

    y = 0
    for r, cells in enumerate(drawn):
        h = row_h[r]
        for c, (tag, img, vox) in enumerate(cells):
            x = LABEL_W + c * cell_w
            sheet.paste(img, (x + (cell_w - img.width) // 2,
                              y + (h - 26 - img.height)), img)
            d.text((x + 8, y + h - 20), f"{tag}   {vox:,} vox", (198, 202, 210))
        d.text((8, y + h // 2), f"{PX_PER_M:g} px/m", (235, 238, 244))
        y += h

    OUT.parent.mkdir(exist_ok=True)
    sheet.save(OUT)
    print(f"{OUT}   {sheet.width}x{sheet.height}   one scale: {PX_PER_M:g} px/m")


if __name__ == "__main__":
    main()
