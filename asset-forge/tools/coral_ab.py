"""The seven 2 cm corals at 2 cm against 5 cm, same ground, side by side.

WHY THIS SHEET EXISTS. Owner decision 2026-08-27: everything in the game divides
down from 10 cm through 5 cm to 2.5 cm, so the seven coral species authored at
2 cm move to 5 cm. Nothing in voxel-core resamples an AssetGrid
(asset-forge/README.md:38-41), so that move is a REBUILD at a different pitch,
not a conversion -- the shapes change. Whether they are still the same coral is
a look judgement and the owner is the only one who can make it.

TWO CONDITIONS TO READ IT WITH, and neither is a hedge:

1. THE 5 cm COLUMN IS A FLOOR, NOT A VERDICT. Only `resolution_cm` changes here.
   That is the mechanical carry-forward, not an authoring pass -- lattice_ab.py
   makes the same point about its own 10 cm column. A real pass would retune
   element counts and thicknesses for the coarser lattice and would do better
   than this. So: if the 5 cm column already looks acceptable, the move is safe.
   If it looks thin, that is an argument for tuning the seven specs, NOT
   necessarily an argument against 5 cm.

2. THESE SEVEN DRAW NOTHING TODAY. They are refused at load because their pitch
   is not 50 mm, so this is not "before and after" in the sense of a change the
   player would notice as a difference. The left column has never been in the
   game. Moving them to 5 cm makes them APPEAR for the first time.

SAME PHYSICAL SCALE, WHICH IS THE WHOLE VALIDITY OF THE SHEET. Pixels-per-voxel
is set proportional to pitch (1 px per cm of voxel edge), so metres-per-pixel is
identical by construction, then ONE common downscale per row fits the cell.
lattice_ab.py earned this the hard way: a per-variant target size made the coarse
column look better than it was, which is the exact direction that would bias this
decision too.
"""
import sys

from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
from forge import pipeline, render, spec as sm

ROOT = _path.ROOT
CELL = 330
SEED = 3

# The seven that ship off the ladder. Read from the banks rather than typed, in
# the export gate's own terms, so this sheet cannot drift from what it is about.
SPECIES = [
    "black-coral-tree",
    "branching-stony-coral",
    "carnation-soft-coral",
    "cold-water-coral",
    "elkhorn-coral",
    "leather-coral",
    "staghorn-coral",
]


def main():
    out = ROOT / "out" / (sys.argv[1] if len(sys.argv) > 1 else "coral-ab.png")
    rows = []
    for name in SPECIES:
        s, _ = sm.load(ROOT / "specs" / f"{name}.json")
        built = []
        for tag, cm in (("authored 2 cm (today, refused)", "2"),
                        ("authored 5 cm (proposed)", "5")):
            spec, _ = sm.patch(s, {"resolution_cm": cm})
            a = pipeline.build(spec, SEED)
            built.append((tag, a))
        rows.append((name, built))

    sheet = Image.new("RGB", (2 * CELL + 210, len(rows) * (CELL + 26)), (24, 25, 28))
    d = ImageDraw.Draw(sheet)
    for r, (label, built) in enumerate(rows):
        y = r * (CELL + 26)
        d.text((8, y + CELL // 2), label, (235, 238, 244))
        # 1 px per cm of voxel edge -- 2 cm -> 2, 5 cm -> 5. Identical
        # metres-per-pixel by construction.
        imgs = [render.render(a.grid, scale=max(1, round(a.grid.voxel_m * 100)), ao=True)
                for _, a in built]
        fit = min(1.0, (CELL - 40) / max(i.width for i in imgs),
                  (CELL - 40) / max(i.height for i in imgs))
        if fit < 1.0:
            imgs = [i.resize((max(1, int(i.width * fit)), max(1, int(i.height * fit))),
                             Image.NEAREST) for i in imgs]
        for c, ((tag, a), img) in enumerate(zip(built, imgs)):
            x = 210 + c * CELL
            sheet.paste(img, (x + (CELL - img.width) // 2,
                              y + (CELL - img.height) - 8), img)
            d.text((x + 8, y + CELL + 6),
                   f"{tag}   {a.stats['voxels']:,} vox   "
                   f"{a.stats['height_m']:.2f} m", (198, 202, 210))
    out.parent.mkdir(exist_ok=True)
    sheet.save(out)
    print(out)

    # The numbers, printed as well as drawn -- a shape judgement is the owner's,
    # but the voxel counts are a fact and belong in the log where they can be
    # quoted without re-reading an image.
    print()
    print("%-26s %10s %10s %8s" % ("species", "2 cm vox", "5 cm vox", "ratio"))
    for label, built in rows:
        a2 = built[0][1].stats["voxels"]
        a5 = built[1][1].stats["voxels"]
        print("%-26s %10s %10s %7.2fx" % (label, f"{a2:,}", f"{a5:,}",
                                          (a2 / a5) if a5 else 0.0))


if __name__ == "__main__":
    main()
