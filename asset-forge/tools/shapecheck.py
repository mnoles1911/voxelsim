"""Render a spec with every solid voxel forced to one bright material.

Separates SHAPE from COLOUR. Rock and bedrock preview colours are dark enough
that on the dark sheet background only the lit top faces show, which reads as a
flat wedge whatever the silhouette actually is.

Rocks are drawn in SIDE ELEVATION, for the reason in `tools/elevation.py`: the
isometric camera looks down into the gaps of a stack and under nothing. Pass
`--iso` for the old camera, `--both` for the pair.

    python tools/shapecheck.py stones.png granite-boulder summit-tor
    python tools/shapecheck.py stones.png summit-tor --both
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
import elevation as ev
from forge import materials, pipeline, spec as sm

ROOT = Path(__file__).resolve().parents[1]
CELL = 320


def main():
    args, flags, opts = ev.split_args(sys.argv[1:])
    out = ROOT / "out" / args[0]
    names = args[1:]

    built = []
    for n in names:
        s, _ = sm.load(ROOT / "specs" / f"{n}.json")
        a = pipeline.build(s, opts["seed"])
        a.grid.data[a.grid.data != 0] = materials.MAT_SAND
        built.append((n, s, a))

    # Everything at ONE scale, then one shrink for the whole sheet, so the
    # stones stay comparable in size. `target_px` sizes the PROJECTION, not the
    # canvas it comes back on, so a large asset returns an image far bigger
    # than the cell -- which is how a 13 m boulder rendered as wallpaper.
    tiles = []
    for camera in ev.CAMERAS:
        group = [r for r in built if camera in ev.cameras_for(flags, r[1])]
        if not group:
            continue
        # Azimuth per asset, measured (`forge.render.best_turn`), because a
        # fixed one has now hidden the defining feature of three assets.
        grids = [ev.turned(a.grid, ev.turn_for(a.grid, flags, opts, s))
                 for _, s, a in group]
        sc = ev.common_scale(grids, camera=camera,
                             target_px=CELL - 30, tilt_deg=opts["tilt"])
        imgs = [ev.view(g, camera, scale=sc, target_px=CELL - 30,
                        tilt_deg=opts["tilt"], ao=True) for g in grids]
        for (n, _, a), img in zip(group, ev.fit(imgs, CELL - 16)):
            tiles.append((f"{n}  {camera}", img, a.stats))

    cols = min(6, len(tiles))
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * CELL, rows * (CELL + 22)), (200, 200, 205))
    d = ImageDraw.Draw(sheet)
    for i, (n, img, st) in enumerate(tiles):
        cx, cy = (i % cols) * CELL, (i // cols) * (CELL + 22)
        sheet.paste(img, (cx + (CELL - img.width) // 2,
                          cy + (CELL - img.height) // 2), img)
        # A fish is measured nose to tail; everything else ground to top.
        size = (f"{st.get('length_m', 0):.2f} m long" if st.get("kind") in ("fish", "cetacean")
                else f"{st['height_m']:.1f} m")
        d.text((cx + 8, cy + CELL + 4), f"{n}  {size}", (20, 20, 24))
    out.parent.mkdir(exist_ok=True)
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()
