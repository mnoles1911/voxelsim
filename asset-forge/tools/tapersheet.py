"""Before and after the taper term, same species, SAME INDIVIDUAL, one locked scale.

The "before" side is not an old checkout. It is the generator with both of this
pass's shape changes turned off in the two places they live:

* `trunk.taper` 0, which is an exact no-op through `skeleton._radii` -- the
  taper is a ceiling on a radius that is already at its maximum at the root, so
  the minimum changes nothing anywhere. And taper is in `spec.SEED_EXCLUDED`, so
  turning it off does NOT change which individual gets built.
* `skeleton.FLARE_REACH_MAX_M` raised out of the way, which restores the old
  flare exactly: the reach becomes `min(H/8, max(huge, 2r))` = `H/8`, and the
  curve is the same expression it always was.

So the two sides differ by this pass and by nothing else -- not by a seed, not
by a rebuild, not by a different tree.

ONE LOCKED SCALE, for the reason `tools/plantsheet.py` gives: the scale is
computed from whichever of the two builds is larger and passed to both, then ONE
resize factor is applied to the pair, and both are bottom-aligned on a common
ground line. Fitting each tile to its own cell silently undoes the comparison,
because a tree that got thinner also gets a larger fit factor and comes back
looking identical -- which is exactly what this sheet exists to show.

    python tools/tapersheet.py out/taper/ab.png european-beech scots-pine
    python tools/tapersheet.py out/taper/ab.png --kind tree --cols 6
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
from forge import pipeline, render, skeleton, spec as sm
from plantprobe import measure

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"
CELL_W, CELL_H = 300, 380
BG = (236, 236, 240)
OLD_FLARE = 1e9      # anything past H/8 restores the old, height-scaled reach


def build(name: str, seed: int, before: bool):
    s, _ = sm.load(SPECS / f"{name}.json")
    # Both sides go through `patch` -- see tools/trunkform.py:run for the
    # non-idempotent-validate bug that makes this necessary.
    want = 0.0 if before else float(sm.get(s, "trunk.taper"))
    s, rep = sm.patch(s, {"trunk.taper": want})
    assert not rep.warnings, (name, rep.warnings)
    keep = skeleton.FLARE_REACH_MAX_M
    if before:
        skeleton.FLARE_REACH_MAX_M = OLD_FLARE
    try:
        a = pipeline.build(s, seed)
    finally:
        skeleton.FLARE_REACH_MAX_M = keep
    return a, measure(a.grid.data, a.stats["voxel_cm"] / 100.0)


def pair(name: str, seed: int, target_px: int):
    a_old, m_old = build(name, seed, before=True)
    a_new, m_new = build(name, seed, before=False)
    sc = max(int(min(render.pick_scale(a_old.grid, target_px),
                     render.pick_scale(a_new.grid, target_px))), 1)
    return [(lbl, render.render(a.grid, scale=sc, ao=True), m, a)
            for lbl, a, m in (("before", a_old, m_old), ("after", a_new, m_new))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("names", nargs="*")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cols", type=int, default=6)
    a = ap.parse_args()

    cells = []
    for n in sorted(set(a.names)):
        try:
            p = pair(n, a.seed, CELL_W - 30)
        except Exception as exc:                       # noqa: BLE001
            print(f"  {n}: FAILED {exc}")
            continue
        cells.append((n, p))
        print(f"  {n:22s} DBH {p[0][2]['dbh_cm']:6.1f} -> {p[1][2]['dbh_cm']:6.1f} cm   "
              f"voxels {p[0][3].stats['voxels']:>9,} -> {p[1][3].stats['voxels']:>9,}",
              flush=True)

    if not cells:
        return
    cols = min(a.cols, len(cells))
    rows = -(-len(cells) // cols)
    sheet = Image.new("RGB", (cols * CELL_W, rows * (2 * CELL_H + 24)), BG)
    d = ImageDraw.Draw(sheet)
    for i, (n, p) in enumerate(cells):
        cx, cy = (i % cols) * CELL_W, (i // cols) * (2 * CELL_H + 24)
        d.text((cx + 6, cy + 4), n, (10, 10, 14))
        mw = max(im.width for _, im, _, _ in p)
        mh = max(im.height for _, im, _, _ in p)
        f = min((CELL_W - 8) / mw, (CELL_H - 22) / mh, 1.0)
        for r, (lbl, img, m, asset) in enumerate(p):
            top = cy + 20 + r * CELL_H
            if f < 1.0:
                img = img.resize((max(1, int(img.width * f)),
                                  max(1, int(img.height * f))), Image.LANCZOS)
            sheet.paste(img, (cx + (CELL_W - img.width) // 2,
                              top + (CELL_H - 22 - img.height)), img)
            d.text((cx + 6, top + CELL_H - 20),
                   f"{lbl}  H {m['height_m']:.1f} m  DBH {m['dbh_cm']:.0f} cm  "
                   f"{asset.stats['voxels']:,} vox", (40, 40, 46))
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    print(f"\nwrote {out}  ({len(cells)} pairs, seed {a.seed})")


if __name__ == "__main__":
    main()
