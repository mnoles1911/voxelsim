"""Before and after the trunk fit, same species, same seed, ONE locked scale.

`tools/crown_ab.py` did this for the crown change and sized each tile to fit,
which is right for judging shape and wrong for judging thickness -- a tree that
got thinner also got a bigger `target_px` scale and came back looking the same.
So here the scale is computed once from the LARGER of the two builds and passed
to both, and both are bottom-aligned on a common ground line. A pixel is the
same number of centimetres on both sides of every pair.

Before-specs are read from `.backup/specs-plantfit-20260815`, the snapshot taken
immediately before `tools/plantfit.py fit --apply`.

    python tools/plantsheet.py out/plantfit-ab.png american-beech douglas-fir
    python tools/plantsheet.py out/plantfit-ab.png --all --seed 2
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw

import _path  # noqa: F401  (sys.path bootstrap)
from forge import pipeline, render, spec as sm
from plantprobe import measure

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT / ".backup" / "specs-plantfit-20260815"
NEW = ROOT / "specs"
CELL_W, CELL_H = 300, 380
BG = (236, 236, 240)


def build(path: Path, seed: int):
    s, _ = sm.load(path)
    a = pipeline.build(s, seed)
    m = measure(a.grid.data, a.stats["voxel_cm"] / 100.0)
    return a, m


def pair(name: str, seed: int, target_px: int):
    old_p, new_p = OLD / f"{name}.json", NEW / f"{name}.json"
    if not old_p.exists():
        return None
    a_old, m_old = build(old_p, seed)
    a_new, m_new = build(new_p, seed)
    # One scale for both sides, from whichever build is bigger.
    sc = min(render.pick_scale(a_old.grid, target_px),
             render.pick_scale(a_new.grid, target_px))
    sc = max(int(sc), 1)
    return [(lbl, render.render(a.grid, scale=sc, ao=True), m)
            for lbl, a, m in (("before", a_old, m_old), ("after", a_new, m_new))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("names", nargs="*")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cols", type=int, default=6)
    a = ap.parse_args()

    names = list(a.names)
    if a.all:
        names += sorted(p.stem for p in OLD.glob("*.json"))
    names = sorted(set(names))

    cells = []
    for n in names:
        try:
            p = pair(n, a.seed, CELL_W - 30)
        except Exception as exc:
            print(f"  {n}: FAILED {exc}")
            continue
        if p is None:
            print(f"  {n}: no before-spec in {OLD.name}")
            continue
        cells.append((n, p))
        print(f"  {n:22s} DBH {p[0][2]['dbh_cm']:6.1f} -> {p[1][2]['dbh_cm']:6.1f} cm")

    if not cells:
        return
    cols = min(a.cols, len(cells))
    rows = -(-len(cells) // cols)
    W, H = cols * CELL_W, rows * (2 * CELL_H + 24)
    sheet = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(sheet)
    for i, (n, p) in enumerate(cells):
        cx = (i % cols) * CELL_W
        cy = (i // cols) * (2 * CELL_H + 24)
        d.text((cx + 6, cy + 4), n, (10, 10, 14))
        # ONE resize factor for the pair, from whichever tile is bigger.
        # Fitting each tile to the cell on its own undoes the locked scale --
        # a tree that got thinner also got a bigger fit factor and came back
        # looking identical, which is the exact failure this sheet exists to
        # avoid.
        mw = max(im.width for _, im, _ in p)
        mh = max(im.height for _, im, _ in p)
        f = min((CELL_W - 8) / mw, (CELL_H - 22) / mh, 1.0)
        for r, (lbl, img, m) in enumerate(p):
            top = cy + 20 + r * CELL_H
            if f < 1.0:
                img = img.resize((max(1, int(img.width * f)),
                                  max(1, int(img.height * f))), Image.LANCZOS)
            # bottom-aligned: both builds sit on the same ground line
            sheet.paste(img, (cx + (CELL_W - img.width) // 2,
                              top + (CELL_H - 22 - img.height)), img)
            d.text((cx + 6, top + CELL_H - 20),
                   f"{lbl}  H {m['height_m']:.1f} m  DBH {m['dbh_cm']:.0f} cm",
                   (40, 40, 46))
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    print(f"\nwrote {out}  ({len(cells)} pairs, seed {a.seed})")


if __name__ == "__main__":
    main()
