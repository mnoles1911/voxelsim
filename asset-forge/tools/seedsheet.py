"""Contact sheet of ONE species across many seeds, for picking an individual.

`sheet.py` answers "do these species look right"; this answers "which of these
individuals do I want". They are different questions and the second one only
comes up for assets there is exactly one of in the world -- a landmark. A hero
vista arch is not a species with a population, it is one rock at one place, so
somebody has to look at half a dozen candidates and choose.

The tile label carries the two numbers that decide it for an arch: height, and
DAYLIGHT -- enclosed background in the silhouette, which is the sky you can see
through the hole. Daylight is what separates an arch from a stone with a dent,
and it is not the same thing as the carve reporting success: `hero-arch-colossal`
seeds 5 and 6 both report an opening most of the width of the face and come out
with 284 px and 7 px of daylight, because the gap breaks out through the
outline instead of closing back over. That is a notch, not a span.

Rendering is deliberately cheap here. Pick at a coarse lattice, bake the winner
at the authored one:

    python tools/seedsheet.py hero-arch-colossal --seeds 6 --res 20
    python tools/seedsheet.py granite-boulder --seeds 8
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
import elevation as ev
from forge import pipeline, render, rock, spec as sm

ROOT = Path(__file__).resolve().parents[1]
CELL = 340


def main() -> int:
    name = sys.argv[1]
    argv = sys.argv[2:]
    count = int(argv[argv.index("--seeds") + 1]) if "--seeds" in argv else 6
    res = float(argv[argv.index("--res") + 1]) if "--res" in argv else None
    bright = "--bright" in argv

    s, _ = sm.load(ROOT / "specs" / f"{name}.json")
    camera = ev.camera_for(s)
    # ANY ANIMAL, not just a fish. This read `== "fish"` and every column it
    # gates -- size quoted as length rather than height, depth instead of
    # daylight -- silently reverted to the stone reading for whales, dolphins
    # and birds. A bird sheet quoted a 24 cm robin as "0.2 m" of HEIGHT with a
    # daylight column of zeroes beside it.
    fish = sm.get(s, "kind") in ("fish", "cetacean", "bird", "quadruped")

    built = []
    for seed in range(1, count + 1):
        try:
            a = pipeline.build(s, seed, resolution_cm=res)
        except MemoryError as e:
            # Named on the sheet rather than skipped. A seed that cannot build
            # is a fact about the candidate, and a gap in a contact sheet reads
            # as "not generated yet".
            print(f"  seed {seed}: DOES NOT BUILD -- {e}", flush=True)
            built.append((seed, None, 0, 0.0))
            continue
        occ = a.grid.data != 0
        # Along both horizontal axes: an arch is a hole from one direction and
        # a wall from ninety degrees round, and the best view is the one that
        # shows the opening.
        #
        # ONLY FOR STONE. Daylight is the arch measure and a fish has none, so
        # on a fish this printed a column of zeroes beside a "height" that was
        # really the animal's depth -- two numbers that look like measurements
        # and are not. A fish is picked on its length and its depth instead.
        day = (0 if fish else
               max(int(rock.daylight(occ.any(axis=ax)).sum()) for ax in (0, 1)))
        if bright:
            from forge import materials
            a.grid.data[occ] = materials.MAT_SAND
        size = (a.stats.get("length_m", 0.0) if fish else a.stats["height_m"])
        print(f"  seed {seed}: {size:.2f} m  {a.stats['voxels']:,} vox  "
              + (f"depth {a.grid.shape[2]} vox" if fish
                 else f"daylight {day:,} px"), flush=True)
        built.append((seed, a.grid, day, size))

    # Through `ev.turn_for`, NOT `render.best_turn` directly. The measured turn
    # scores daylight through the silhouette and then overhang, which is a
    # question about rocks -- an arch is a hole from one direction and a wall
    # from ninety degrees round. On anything else it picks between four views
    # on noise, and on an animal it is actively wrong: a fish or a bird is
    # BUILT facing its camera, and the measure will sometimes turn it end-on.
    # `turn_for` has said "rocks only" since it was written; this tool was
    # calling past it, which is how a sheet of perched songbirds came out
    # showing several of them head-on.
    grids = [ev.turned(g, ev.turn_for(g, [], {"turn": None}, s))
             for _, g, _, _ in built if g]
    if not grids:
        print("nothing built", file=sys.stderr)
        return 1
    sc = ev.common_scale(grids, camera=camera, target_px=CELL - 30)
    imgs = ev.fit([ev.view(g, camera, scale=sc, target_px=CELL - 30)
                   for g in grids], CELL - 16)

    cols = min(3, len(built))
    rows = (len(built) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * CELL, rows * (CELL + 22)), (24, 25, 28))
    d = ImageDraw.Draw(sheet)
    it = iter(imgs)
    for i, (seed, g, day, h) in enumerate(built):
        cx, cy = (i % cols) * CELL, (i // cols) * (CELL + 22)
        if g is None:
            d.text((cx + 8, cy + CELL // 2), f"seed {seed}: did not build",
                   (220, 120, 120))
            continue
        img = next(it)
        sheet.paste(img, (cx + (CELL - img.width) // 2,
                          cy + (CELL - img.height) // 2), img)
        d.text((cx + 8, cy + CELL + 4),
               f"seed {seed}   {h:.2f} m"
               + (f"   {g.shape[0]}x{g.shape[2]} vox" if fish
                  else f"   daylight {day:,} px"),
               (200, 202, 208))

    out = ROOT / "out" / f"{name}-seeds.png"
    out.parent.mkdir(exist_ok=True)
    sheet.save(out)
    print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
