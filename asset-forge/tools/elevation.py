"""Review-camera glue for the rock tools.

The projection itself lives in `forge.render.elevation`, not here, for one
reason: the browser gallery (`forge/server.py`) needs the same camera, and it
cannot import out of `tools/`. What is left in this module is the part that is
genuinely command-line -- which flags mean what, one scale for a whole sheet,
and choosing a view per asset.

    --side / --iso / --broad / --both   camera; the default is per kind
    --bright                  force one light material, to judge shape not colour
    --tilt N                  camera height in degrees above level (default 10)
    --seed N                  which individual
    --turn N                  force a quarter turn; the default is measured,
                              the turn that shows the most (`best_turn`)
    --noturn                  never turn, whatever the measure says

THE DEFAULT CAMERA IS `forge.render.camera_for` AND NOTHING HERE DECIDES IT.
This module used to answer the question itself with an `is_rock` test while the
browser gallery answered it with a `kind in BOULDER_KINDS` test, which is two
copies of one rule -- and they had already drifted: the gallery showed a rock
from the side and the detail overlay showed the same rock from the isometric.
"""
from __future__ import annotations

from PIL import Image

import _path  # noqa: F401  (sys.path bootstrap)
from forge import render as _r
from forge.grid import VoxelGrid

DEFAULT_TILT_DEG = _r.ELEVATION_TILT_DEG
CAMERAS = _r.CAMERAS      # ("iso", "side", "broad"); see forge.render.camera_for


turned = _r.turned  # quarter turns about the vertical; see forge.render.turned


def view(grid: VoxelGrid, camera: str, *, target_px: int,
         tilt_deg: float | None = None, ao: float = 0.45,
         scale: int | None = None, background=None) -> Image.Image:
    """Draw through one of the three cameras, so callers stay one line.

    `background=None` leaves the image transparent outside the asset, which is
    what a contact sheet wants: an opaque backdrop pastes a black rectangle
    into every cell and the silhouette -- the thing being judged -- is then
    read against black instead of against the sheet.
    """
    return _r.view(grid, camera, scale=scale, target_px=target_px,
                   tilt_deg=tilt_deg, ao=ao, background=background)


def common_scale(grids, *, camera: str, target_px: int,
                 tilt_deg: float | None = None) -> int:
    """One rendering scale for a whole sheet.

    Without this each cell picks the largest scale that fits its own box, so a
    0.3 m scree chip and a 6.4 m tor come out nearly the same size on the page,
    and the sheet hides the one property that is hardest to judge any other
    way. `forge.render.scale_for` has said this since it was written; the tools
    were calling it one grid at a time, which defeats it.
    """
    return _r.scale_for_camera([g.shape for g in grids], camera, target_px,
                               tilt_deg)


def fit(images, box: int):
    """Shrink a set of images by ONE factor so they still compare by size."""
    if not images:
        return images
    w = max(i.width for i in images)
    h = max(i.height for i in images)
    f = min(1.0, box / max(w, 1), box / max(h, 1))
    if f >= 1.0:
        return images
    return [i.resize((max(1, int(i.width * f)), max(1, int(i.height * f))),
                     Image.LANCZOS) for i in images]


def is_rock(spec: dict) -> bool:
    from forge.spec import get
    return get(spec, "kind") == "rock"


def camera_for(spec: dict) -> str:
    """The default camera for this asset. One definition; see forge.render."""
    return _r.camera_for(spec)


def split_args(argv):
    """Positional arguments, flag set, and valued options, from a command line.

    Shared by the review tools so the flags mean the same thing in all of them.
    """
    args, flags = [], set()
    # `tilt` defaults to None, not to the elevation camera's 10 degrees:
    # each camera has its own right angle and passing one camera's default
    # to another silently overrides it.
    opts = {"tilt": None, "seed": 1, "turn": None}
    valued = {"--tilt": ("tilt", float), "--seed": ("seed", int),
              "--turn": ("turn", int)}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in valued and i + 1 < len(argv):
            key, cast = valued[a]
            opts[key] = cast(argv[i + 1])
            i += 2
            continue
        (flags.add(a) if a.startswith("--") else args.append(a))
        i += 1
    return args, flags, opts


def cameras_for(flags, spec) -> list[str]:
    """Which cameras to draw this asset through.

    The default is the asset kind's own camera. A flag forces one: a rock from
    the isometric is a fair question to ask, it is just the wrong default.
    """
    if "--both" in flags:
        return [camera_for(spec), "iso"] if camera_for(spec) != "iso" else ["iso"]
    for flag, cam in (("--iso", "iso"), ("--side", "side"), ("--broad", "broad")):
        if flag in flags:
            return [cam]
    return [camera_for(spec)]


def turn_for(grid: VoxelGrid, flags, opts, spec=None) -> int:
    """Which quarter turn a single-still tile should use.

    Default is measured, not fixed: `forge.render.best_turn` picks the turn
    that shows the most daylight, then the most overhang. An arch is a hole
    from one direction and a wall from ninety degrees round -- 65% open sky
    against 0.1% on `hero-natural-arch` -- and a fixed camera showed the wall.

    Rocks only. A tree has no defining hole and no undercut, so the measure
    would be picking between four equivalent views on noise, and a contact
    sheet whose trees turn between runs is harder to compare, not easier. A
    fish is the strongest case of all for never turning: it has ONE correct
    view, it is built facing it, and a measured turn would sometimes show the
    animal end-on.
    """
    if opts.get("turn") is not None:
        return int(opts["turn"]) % 4
    if "--noturn" in flags or (spec is not None and not is_rock(spec)):
        return 0
    return _r.best_turn(grid.data)
