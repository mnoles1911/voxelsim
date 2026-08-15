"""Before, after and the reference, side by side, at ONE scale and ONE camera.

The owner judges renders. This file exists so that what he judges is a
comparison and not two pictures taken at different sizes on different days --
`docs/` records a whole afternoon lost to a "shape bug" that was a dark palette
and another to Chrome's 500-pixel window floor, and both would have been
one glance if the two pictures had been the same size.

SO THE SCALE IS SOLVED ONCE, ACROSS BOTH BUILDS, AND THEN FORCED. A voxel scale
picked per-render is picked from that render's own extent, and this change makes
animals THICKER -- so the after would be drawn at a smaller scale than the
before, and a genuine 50% limb change would arrive on screen looking like no
change at all. That is not a hypothetical: it is the same failure as measuring a
ratio whose denominator moves, which cost this project a day on trunk girth.

    python tools/refsheet.py --before out/reffit/before   # run BEFORE fitting
    python tools/refsheet.py --after  out/reffit/after    # run AFTER fitting
    python tools/refsheet.py --sheet  out/reffit/sheets   # compose the three
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image

import _path  # noqa: F401
import refsil
from forge import pipeline, render, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"
REFS = ROOT / "refs"
SILS = REFS / "silhouettes"
OUT = ROOT / "out" / "reffit"
SEED = 1
PX = 460


def _names(only) -> list[str]:
    if only:
        return list(only)
    f = OUT / "changed.txt"
    if f.exists():
        return f.read_text(encoding="utf-8").split()
    raise SystemExit("no species given and no out/reffit/changed.txt")


def _scale_file(n: str) -> Path:
    return OUT / "scales.json"


def shoot(names: list[str], dest: Path, lock: bool) -> int:
    """Render each species. `lock` writes the chosen scale; otherwise it reads
    it, so the after uses the BEFORE's scale and the two are comparable."""
    dest.mkdir(parents=True, exist_ok=True)
    sf = _scale_file("")
    scales = json.loads(sf.read_text(encoding="utf-8")) if sf.exists() else {}
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        spec, _ = sm.patch(spec, {"variation.amount": 0.0})
        a = pipeline.build(spec, SEED)
        grid = a.grid if hasattr(a, "grid") else a
        cam = render.camera_for(spec)
        if lock or n not in scales:
            s = render.scale_for_camera([render.grid_extent(grid)], cam, PX)
            scales[n] = {"scale": int(s), "camera": cam}
        img = render.view(grid, scales[n]["camera"], scale=scales[n]["scale"])
        img.save(dest / f"{n}.png")
        print(f"{n:<24} {scales[n]['camera']:<10} scale {scales[n]['scale']} "
              f"-> {dest.name}/{n}.png")
    sf.write_text(json.dumps(scales, indent=2, sort_keys=True) + "\n",
                  encoding="utf-8")
    return 0


def _sil_png(m, h: int) -> Image.Image:
    """A silhouette as grey on white, at a given height."""
    import numpy as np
    m = refsil._crop(m)
    w, hh = m.shape
    body = m.T[::-1]
    arr = np.full((hh, w, 3), 255, np.uint8)
    arr[body] = (90, 90, 90)
    im = Image.fromarray(arr)
    return im.resize((max(1, int(w * h / hh)), h), Image.LANCZOS)


def _asset_sil(n: str, h: int) -> Image.Image | None:
    spec, _ = sm.load(SPECS / f"{n}.json")
    spec, _ = sm.patch(spec, {"variation.amount": 0.0})
    a = pipeline.build(spec, SEED)
    data = a.grid.data if hasattr(a, "grid") else a.data
    return _sil_png(refsil.from_grid(data), h)


def _ref_image(n: str, h: int) -> Image.Image | None:
    """One reference silhouette, drawn light-on-white at the render's height.

    Only silhouettes the extractor actually USED are shown. Showing a refused
    one beside a fitted asset would put a picture next to a number that was not
    taken from it.
    """
    ex = REFS / "quadruped-reference.json"
    if not ex.exists():
        return None
    rec = json.loads(ex.read_text(encoding="utf-8")).get(n)
    if not rec or not rec.get("used"):
        return None
    p = SILS / rec["used"][0]
    if not p.exists():
        return None
    return _sil_png(refsil.from_png(p), h)


def sheet(names: list[str], dest: Path) -> int:
    dest.mkdir(parents=True, exist_ok=True)
    for n in names:
        parts = []
        for tag in ("before", "after"):
            p = OUT / tag / f"{n}.png"
            if p.exists():
                parts.append((tag, Image.open(p).convert("RGB")))
        if not parts:
            continue
        h = max(im.height for _, im in parts)
        # ROW 1 IS COLOUR, ROW 2 IS SHAPE, and row 2 is the one that answers the
        # question this change was about. A wolverine is a dark animal on a dark
        # background: `docs/` records two "shape bugs" on this library that were
        # a dark palette and nothing else, and a limb thickness change is
        # invisible in a colour render of a black mustelid. The silhouette row
        # is our own asset and the reference reduced by the SAME code that
        # produced the numbers, so what is being compared on screen is what was
        # compared arithmetically.
        top = [im for _, im in parts]
        bottom = [c for c in (_asset_sil(n, h), _ref_image(n, h)) if c]
        pad = 12
        w = max(sum(c.width for c in row) + pad * (len(row) + 1)
                for row in (top, bottom) if row)
        out = Image.new("RGB", (w, 2 * h + pad * 3), (255, 255, 255))
        for r, row in enumerate((top, bottom)):
            x = pad
            for c in row:
                out.paste(c, (x, pad + r * (h + pad) + (h - c.height) // 2))
                x += c.width + pad
        out.save(dest / f"{n}.png")
        print(f"{n:<24} colour[{', '.join(t for t, _ in parts)}] "
              f"shape[ours{', reference' if len(bottom) > 1 else ''}]"
              f" -> {dest.name}/{n}.png")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--before", action="store_true")
    ap.add_argument("--after", action="store_true")
    ap.add_argument("--sheet", action="store_true")
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    names = _names(a.only)
    if a.before:
        return shoot(names, OUT / "before", lock=True)
    if a.after:
        return shoot(names, OUT / "after", lock=False)
    if a.sheet:
        return sheet(names, OUT / "sheets")
    ap.error("pick --before, --after or --sheet")


if __name__ == "__main__":
    raise SystemExit(main())
