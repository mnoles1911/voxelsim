"""Contact sheet of one KIND, split by the biomes it lives in.

`sheet.py` shows a set you name. This shows a set the WORLD names: every
species of a kind whose biome weight in a given biome is non-zero, one sheet
per biome, with the weight on the label.

A species appears in every biome that hosts it, and that repetition is the
point rather than a fault -- a brown trout is 0.9 temperate forest, 0.6 taiga
and 0.35 tundra, and the question "what swims here" has to answer with all
three or it is not answering.

    python tools/biomesheet.py fish
    python tools/biomesheet.py fish cetacean --out out/fish-by-biome
    python tools/biomesheet.py bird --min 0.3
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
import elevation as ev
from forge import biomes as biomelib, pipeline, render, spec as sm

ROOT = Path(__file__).resolve().parents[1]
CELL = 240


def main() -> int:
    argv = sys.argv[1:]
    kinds = [a for a in argv if not a.startswith("--")]
    out_dir = ROOT / "out" / "by-biome"
    if "--out" in argv:
        out_dir = ROOT / argv[argv.index("--out") + 1]
    min_w = float(argv[argv.index("--min") + 1]) if "--min" in argv else 0.0
    if not kinds:
        print("name at least one kind, e.g. fish", file=sys.stderr)
        return 2
    out_dir.mkdir(parents=True, exist_ok=True)

    # Build every species ONCE. A species in four biomes would otherwise be
    # grown four times, and the big ones are not cheap.
    built = {}
    for p in sorted(ROOT.glob("specs/*.json")):
        s, _ = sm.load(p)
        if sm.get(s, "kind") not in kinds:
            continue
        a = pipeline.build(s, 1)
        built[p.stem] = (s, a)
        print(f"  built {p.stem}", flush=True)

    keys = [b.key if hasattr(b, "key") else b for b in biomelib.BIOMES]
    total = 0
    for bk in keys:
        here = []
        for name, (s, a) in built.items():
            w = float(s.get("biomes", {}).get(bk, 0.0))
            if w > min_w:
                here.append((w, name, s, a))
        if not here:
            print(f"{bk:18} -- nothing")
            continue
        here.sort(key=lambda r: -r[0])

        # One scale in METRES across the sheet: these sets span 1, 2, 5 and
        # 10 cm, and a scale in voxels would make a 20 cm minnow at 1 cm the
        # same size on the page as a 2 m tuna at 10 cm.
        # ONE SCALE IN METRES, and it has to survive to the page.
        #
        # The first version of this computed the scale and then called
        # `thumbnail` on every cell, which resizes each image independently and
        # throws the whole thing away -- a 0.25 m moorish idol and a 26 m blue
        # whale came out the same size on a sheet whose own header said they
        # were to scale. A caption that describes something the picture does
        # not do is worse than no caption.
        #
        # So the scale is chosen from the LARGEST animal in the set, every cell
        # is drawn at it, and anything that still overflows is cropped rather
        # than resized. A 26 m whale beside a 25 cm reef fish means the reef
        # fish is a few pixels. That is the honest picture of an ocean.
        grids = [a.grid for _, _, _, a in here]
        longest_m = max(max(g.shape) * g.voxel_m for g in grids)
        px_per_m = (CELL - 20) / longest_m
        imgs = []
        for _, _, s, a in here:
            scale = max(1, int(round(px_per_m * a.grid.voxel_m)))
            im = ev.view(a.grid, ev.camera_for(s), scale=scale,
                         target_px=CELL * 4)
            if im.width > CELL - 12 or im.height > CELL - 46:
                im = im.crop((0, 0, min(im.width, CELL - 12),
                              min(im.height, CELL - 46)))
            imgs.append(im)

        cols = min(6, len(here))
        rows = (len(here) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * CELL, rows * CELL + 30), (24, 25, 28))
        d = ImageDraw.Draw(sheet)
        d.text((10, 8), f"{bk.replace('_', ' ')}  --  {len(here)} species of "
                        f"{'/'.join(kinds)}, ordered by biome weight",
               (235, 228, 200))
        for i, ((w, name, s, a), im) in enumerate(zip(here, imgs)):
            cx, cy = (i % cols) * CELL, (i // cols) * CELL + 30
            top = cy + 6 + max(0, (CELL - 52 - im.height) // 2)
            sheet.paste(im, (cx + (CELL - im.width) // 2, top), im)
            length = a.stats.get("length_m") or a.stats.get("height_m", 0.0)
            d.text((cx + 6, cy + CELL - 38), name[:30], (222, 224, 230))
            d.text((cx + 6, cy + CELL - 24),
                   f"w {w:.2f}   {length:.2f} m   "
                   f"{int(a.grid.voxel_m * 100)} cm", (150, 153, 162))
        path = out_dir / f"{bk}.png"
        sheet.save(path)
        total += len(here)
        print(f"{bk:18} {len(here):>3} species -> {path}")
    print(f"\n{len(built)} species, {total} biome placements, "
          f"one scale in metres per sheet")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
