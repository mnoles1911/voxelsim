"""Contact sheet of one seed per named spec, for eyeballing shape.

Each kind is drawn through its own camera (`forge.render.camera_for`): trees
and ground cover from the isometric, rocks from a low side elevation, fish
broadside. That is not a preference. The isometric looks down 35 degrees onto
exactly the feature a stone is usually about -- the pinch points of a stack,
the space under an overhang -- and fills it with whatever stands behind;
`hero-tor-stack` was reviewed from that camera and called "overlapping plates",
which it is not. And it lays a fish's own length across its own width.

    python tools/sheet.py rocks.png granite-boulder summit-tor
    python tools/sheet.py fish.png brown-trout river-perch
    python tools/sheet.py rocks.png granite-boulder --both     # kind camera + iso
    python tools/sheet.py trees.png temperate-oak --iso        # force iso
    python tools/sheet.py rocks.png summit-tor --bright        # shape, no colour
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
import elevation as ev
from forge import materials, pipeline, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"
CELL = 300


def _longest_m(a) -> float:
    """The asset's longest dimension in METRES, whichever axis it is on."""
    st = a.stats
    return max(st.get("length_m", 0.0), st.get("height_m", 0.0),
               *(st.get("footprint_m") or (0.0,)))


def _to_metres(imgs, assets, px_per_m: float):
    """Resize a set of tiles so one metre is the same number of pixels.

    `px_per_m` is computed once for the WHOLE sheet and passed in, which is the
    other half of the fix: the sheet renders each camera's group separately, so
    a per-group scale would still put the fish on one ruler and the whales on
    another.
    """
    spans = [max(_longest_m(a), 1e-6) for a in assets]
    out = []
    for im, span in zip(imgs, spans):
        want = max(1, int(round(span * px_per_m)))
        f = want / max(im.width, 1)
        if abs(f - 1.0) < 0.02:
            out.append(im)
            continue
        out.append(im.resize((max(1, int(im.width * f)),
                              max(1, int(im.height * f))), Image.LANCZOS))
    return out


def main():
    args, flags, opts = ev.split_args(sys.argv[1:])
    out = ROOT / "out" / args[0]
    names = args[1:]

    built = []
    for n in names:
        s, _ = sm.load(SPECS / f"{n}.json")
        a = pipeline.build(s, opts["seed"])
        if "--bright" in flags:
            # Rock preview colours are dark enough that on a dark sheet only
            # the lit top faces show, which reads as a flat wedge whatever the
            # silhouette is. A dark tile is not a shape bug.
            a.grid.data[a.grid.data != 0] = materials.MAT_SAND
        built.append((n, s, a))

    # One scale for the whole sheet, then one shrink, so a sapling and an
    # emergent are not the same size on the page. `target_px` sizes the
    # PROJECTION, not the canvas it comes back on, so anything tall returns an
    # image bigger than the cell and a per-cell paste crops it to a close-up of
    # the middle -- which is how four trees rendered as four patches of leaf.
    # ONE SCALE MEANS ONE SCALE IN METRES, NOT IN VOXELS.
    #
    # `common_scale` picks one pixels-per-VOXEL multiplier for a group, which is
    # the same thing as one scale in metres only while every spec shares a
    # lattice. That stopped being true the day `resolution_cm` became a
    # per-species choice: a 5.45 m great white at 5 cm is 109 voxels long and a
    # 2.84 m bluefin tuna at 2 cm is 142, so on a voxel-scaled sheet the TUNA
    # IS THE BIGGER FISH. The first aquatic sheet put a 0.24 m clownfish and a
    # 29 m blue whale at roughly the same width and read as though that were
    # fine.
    #
    # So when a sheet spans more than one lattice, every tile is resized after
    # rendering so that a metre is the same number of pixels everywhere. The
    # header says which mode is in use, because a sheet that silently changes
    # what its sizes mean is worse than one that gets them wrong loudly.
    lattices = {float(sm.get(sp, "resolution_cm")) for _, sp, _ in built}
    by_metres = len(lattices) > 1 and "--fit" not in flags
    mode = ("one scale in METRES (specs span "
            + ", ".join(f"{c:g}" for c in sorted(lattices)) + " cm)" if by_metres
            else "one scale per cell" if "--fit" in flags
            else f"one scale in voxels ({sorted(lattices)[0]:g} cm throughout)")
    print(f"  scaling: {mode}")
    # ONE pixels-per-metre for the WHOLE sheet, computed before the camera
    # groups are split. Computing it per group would put the fish on one ruler
    # and the whales on another, which is the bug this whole block exists to
    # fix, one level up.
    px_per_m = ((CELL - 16) / max(_longest_m(a) for _, _, a in built)
                if by_metres else 0.0)

    tiles = []
    for camera in ev.CAMERAS:
        group = [r for r in built if camera in ev.cameras_for(flags, r[1])]
        if not group:
            continue
        # One still per asset, so the azimuth is chosen by measurement rather
        # than fixed: an arch is a hole from one direction and a wall from
        # ninety degrees round, and the fixed camera was showing the wall.
        grids = [ev.turned(a.grid, ev.turn_for(a.grid, flags, opts, s))
                 for _, s, a in group]
        tilt = opts["tilt"]
        if "--fit" in flags:
            # One scale per cell, for judging SHAPE rather than size.
            #
            # The common scale below is the right default and it stays the
            # default: it is what stops a sapling and an emergent occupying the
            # same square. But it makes a sheet that spans the library
            # unreadable at the small end -- a 0.7 m coastal scrub next to a
            # 31 m emergent is a dozen pixels, and no amount of squinting tells
            # you whether its foliage is right. Two different questions, two
            # flags; the one thing not to do is split the difference and answer
            # neither.
            imgs = [ev.view(g, camera, scale=None, target_px=CELL - 30,
                            tilt_deg=tilt, ao=True) for g in grids]
        else:
            sc = ev.common_scale(grids, camera=camera,
                                 target_px=CELL - 30, tilt_deg=tilt)
            imgs = [ev.view(g, camera, scale=sc, target_px=CELL - 30,
                            tilt_deg=tilt, ao=True) for g in grids]
        # `ev.fit` shrinks the set by ONE factor, which is what preserves the
        # size comparison -- and is also why passing `scale=None` above changed
        # nothing on its own. Under `--fit` each tile is shrunk to the cell
        # independently instead.
        if "--fit" in flags:
            sized = []
            for im in imgs:
                im = im.copy()
                im.thumbnail((CELL - 16, CELL - 16), Image.LANCZOS)
                sized.append(im)
        else:
            sized = ev.fit(imgs, CELL - 16)
        if by_metres:
            sized = _to_metres(sized, [a for _, _, a in group], px_per_m)
        for (n, _, a), img in zip(group, sized):
            tiles.append((f"{n}  {camera}", img, a.stats))

    cols = min(5, len(tiles))
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * CELL, rows * (CELL + 22)), (24, 25, 28))
    d = ImageDraw.Draw(sheet)
    for i, (n, img, st) in enumerate(tiles):
        cx, cy = (i % cols) * CELL, (i // cols) * (CELL + 22)
        sheet.paste(img, (cx + (CELL - img.width) // 2,
                          cy + (CELL - img.height) // 2), img)
        # A fish is measured nose to tail, everything else ground to top.
        size = (f"{st.get('length_m', 0):.2f} m long" if st.get("kind") in ("fish", "cetacean")
                else f"{st['height_m']:.1f} m")
        d.text((cx + 8, cy + CELL + 4),
               f"{n}  {size}  {st['voxels']:,} vox", (200, 202, 208))
    out.parent.mkdir(exist_ok=True)
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()
