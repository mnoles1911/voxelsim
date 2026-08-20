"""Author-for-2cm versus 5cm versus 10cm, same ground, side by side.

The point is the word AUTHOR. Shrinking a 2 cm asset into a 5 cm lattice
answers a different and easier question than designing one for 5 cm, and
answering the easy question is how you end up rejecting a lattice that would
have been fine. Each row here is the same plant tuned once per voxel size, and
rendered at the same physical scale so the comparison is about detail and not
about size on screen.

THE 10 CM COLUMN IS THE WORLD LATTICE, and it is here because
docs/detail-assets-in-the-volume-2026-08-19.md asks whether the 230 detail-plant
species could live in the world volume as it is rather than in a 5 cm volume of
their own. It is authored, not resampled -- nothing in voxel-core resamples an
AssetGrid (asset-forge/README.md:38-41), so a shrink would be measuring a bug
rather than a lattice.

TWO THINGS TO KNOW BEFORE READING IT. At 10 cm a blade is one voxel, i.e. 10 cm
of apparent thickness against a real blade's few millimetres, so the 10 cm
column is not "the same plant, coarser" -- it is the nearest thing the lattice
can hold, and whether that is still the plant is the owner's call, not this
tool's. And the 10 cm parameters below are a mechanical carry-forward of the
2 cm -> 5 cm moves (fewer elements, one-voxel thickness, wider spread); a real
authoring pass would do better, so read this column as a floor on 10 cm quality
rather than a verdict on it.
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
      "tuft.arc": 0.6, "tuft.length_var": 0.4},
     # 10 cm: a blade is one voxel wide. Fewer, thicker, taller, spread wider
     # so the tuft still reads as separate stems rather than a solid slab.
     {"resolution_cm": "10", "height_m": 0.6, "tuft.stems": 5,
      "tuft.width_m": 0.10, "tuft.spread_m": 0.16, "tuft.base_m": 0.16,
      "tuft.arc": 0.6, "tuft.length_var": 0.4}),

    ("reed", "water-reed",
     {"resolution_cm": "2"},
     {"resolution_cm": "5", "tuft.stems": 12, "tuft.width_m": 0.055,
      "tuft.head_m": 0.09, "tuft.spread_m": 0.22, "tuft.base_m": 0.12},
     {"resolution_cm": "10", "tuft.stems": 6, "tuft.width_m": 0.10,
      "tuft.head_m": 0.16, "tuft.spread_m": 0.30, "tuft.base_m": 0.20}),

    ("flower (daisy-size head)", "meadow-daisy",
     {"resolution_cm": "1"},
     {"resolution_cm": "5", "height_m": 0.4, "tuft.stems": 7,
      "tuft.width_m": 0.05, "tuft.head_m": 0.12, "tuft.base_m": 0.07},
     # A daisy head is 2-3 cm across. At 10 cm the head must grow to a single
     # voxel before it exists at all; this is the question the row asks.
     {"resolution_cm": "10", "height_m": 0.6, "tuft.stems": 4,
      "tuft.width_m": 0.10, "tuft.head_m": 0.22, "tuft.base_m": 0.12}),

    ("flower (big head)", "meadow-daisy",
     {"resolution_cm": "1", "tuft.head_m": 0.1, "height_m": 0.5},
     # How big does a bloom have to be before 5 cm can hold a rosette at all?
     {"resolution_cm": "5", "height_m": 0.7, "tuft.stems": 6,
      "tuft.width_m": 0.05, "tuft.head_m": 0.26, "tuft.base_m": 0.07},
     {"resolution_cm": "10", "height_m": 0.9, "tuft.stems": 4,
      "tuft.width_m": 0.10, "tuft.head_m": 0.40, "tuft.base_m": 0.12}),
]


def main():
    out = ROOT / "out" / (sys.argv[1] if len(sys.argv) > 1 else "lattice-ab.png")
    rows = []
    for label, base, fine, mid, coarse in CASES:
        s, _ = sm.load(ROOT / "specs" / f"{base}.json")
        built = []
        for tag, over in (("author 2 cm", fine), ("author 5 cm", mid),
                          ("author 10 cm", coarse)):
            spec, _ = sm.patch(s, over)
            a = pipeline.build(spec, 3)
            built.append((f"{tag} @ {sm.get(spec, 'resolution_cm')} cm", a))
        rows.append((label, built))

    # ONE METRES-PER-PIXEL PER ROW, AND IT HAS TO BE EXACT.
    #
    # This is the whole validity of the sheet: if a 2 cm plant and a 10 cm plant
    # are not drawn at the same physical scale, the reader is comparing size on
    # screen and calling it detail. Measured 2026-08-19, the previous
    # target_px-per-variant picker did exactly that -- render.scale_for quantises
    # pixels-per-voxel to {8,6,4,3,2,1} and each variant was scaled to FILL its
    # cell, so the 5 cm reed came out 63x118 px against the 2 cm reed's 151x345
    # for nearly the same 2.7 m plant. The coarse column looked better than it is.
    #
    # The fix is to stop asking for a target size at all. Pixels-per-voxel is set
    # PROPORTIONAL TO PITCH (1 px per cm of voxel edge: 2 cm -> 2, 5 cm -> 5,
    # 10 cm -> 10), which makes metres-per-pixel identical by construction and
    # stays integral because every authored pitch is a whole number of cm. Then
    # ONE common downscale per row fits the cell -- common, so it cannot
    # reintroduce the bias it just removed.
    sheet = Image.new("RGB", (3 * CELL + 150, len(rows) * (CELL + 24)), (24, 25, 28))
    d = ImageDraw.Draw(sheet)
    for r, (label, built) in enumerate(rows):
        y = r * (CELL + 24)
        d.text((8, y + CELL // 2), label, (235, 238, 244))
        imgs = [render.render(a.grid, scale=max(1, round(a.grid.voxel_m * 100)), ao=True)
                for _, a in built]
        fit = min(1.0, (CELL - 40) / max(i.width for i in imgs),
                  (CELL - 40) / max(i.height for i in imgs))
        if fit < 1.0:
            imgs = [i.resize((max(1, int(i.width * fit)), max(1, int(i.height * fit))),
                             Image.NEAREST) for i in imgs]
        for c, ((tag, a), img) in enumerate(zip(built, imgs)):
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
