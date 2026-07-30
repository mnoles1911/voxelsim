#!/usr/bin/env python3
"""Find ONE specific terrace-boundary line in the voxelised surface, and draw it.

WHY THIS EXISTS. The terracing artifact has been argued about from screenshots for
a long stretch, with the assistant repeatedly wrong about its cause. Before any
more work goes into fixing it, the thing being fixed has to be pinned to a
specific, locatable feature that both parties agree is the same feature. This
takes the raw voxel-quantised surface, finds the single longest straight terrace
boundary in it, prints its WORLD COORDINATES so it can be stood on in game, and
renders the level field so the bands are unmistakable.

INPUT is the raw dump written by vxc_terrainprobe under VXC_PROBE_DUMP: n*n int32
surface heights in millimetres, row-major, sampled at ONE VOXEL spacing starting
at the probe's (vx0, vy0).

WHAT "A CONTOUR LINE" MEANS HERE, precisely, because the whole point is to be
unambiguous: quantise every height to its voxel level, floor(h / 100 mm). A
boundary cell is one whose level differs from its right or lower neighbour. A
straight run is a maximal set of boundary cells that are contiguous along a single
row or column. The longest such run is the feature the eye reads as "a line".

Usage:
  python tools/find_contour_line.py --bin D:\\ue-cache\\g35-surface.bin \
      --vx0 -691200 --vy0 384000 --out D:\\ue-cache\\g35-contours.png
"""
import argparse
import numpy as np
from PIL import Image, ImageDraw

VOXEL_MM = 100


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--n", type=int, default=512)
    # Probe origin in VOXELS (vx0 = xMetres * 1000 / 100).
    ap.add_argument("--vx0", type=int, required=True)
    ap.add_argument("--vy0", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--top", type=int, default=5)
    a = ap.parse_args()

    h = np.fromfile(a.bin, dtype="<i4", count=a.n * a.n).reshape(a.n, a.n).astype(np.int64)
    level = np.floor_divide(h, VOXEL_MM)

    # A CONTOUR LINE IS ONE LEVEL'S BOUNDARY, NOT THE UNION OF ALL OF THEM.
    #
    # The first version of this took every cell whose level differed from a
    # neighbour. On steep ground that is nearly every cell -- at the first site
    # tried, 243,081 of 262,144, because 54 m of relief over 51 m means every
    # column changes level -- so "the longest straight run" measured saturation,
    # not a line. The union of all contours fills the plane; a single contour is a
    # curve. Only the second is the thing the eye reads as a line on a hillside.
    #
    # So: pick the level whose own boundary is longest (the most prominent single
    # contour in the window) and work on that alone.
    best_k, best_b, best_n = None, None, -1
    lo, hi = int(level.min()), int(level.max())
    for k in range(lo + 1, hi + 1):
        inside = level >= k
        bb = np.zeros_like(inside, dtype=bool)
        bb[:, :-1] |= inside[:, :-1] != inside[:, 1:]
        bb[:-1, :] |= inside[:-1, :] != inside[1:, :]
        n = int(bb.sum())
        if n > best_n:
            best_k, best_b, best_n = k, bb, n
    b = best_b
    print(f"most prominent single contour: level {best_k} "
          f"(elevation {best_k * VOXEL_MM / 1000.0:.1f} m), {best_n} boundary cells")

    # Longest straight runs, along rows and along columns.
    runs = []  # (length, kind, j, i0, i1)
    for j in range(a.n):
        i = 0
        while i < a.n:
            if b[j, i]:
                i0 = i
                while i < a.n and b[j, i]:
                    i += 1
                if i - i0 >= 2:
                    runs.append((i - i0, "row", j, i0, i - 1))
            else:
                i += 1
    for i in range(a.n):
        j = 0
        while j < a.n:
            if b[j, i]:
                j0 = j
                while j < a.n and b[j, i]:
                    j += 1
                if j - j0 >= 2:
                    runs.append((j - j0, "col", i, j0, j - 1))
            else:
                j += 1
    runs.sort(reverse=True)

    print(f"level range: {level.min()} .. {level.max()}  "
          f"({(level.max() - level.min()) * VOXEL_MM / 1000.0:.1f} m of relief over "
          f"{a.n * VOXEL_MM / 1000.0:.1f} m)")
    print(f"boundary cells: {int(b.sum())} of {a.n * a.n}")
    print(f"\nTHE {a.top} LONGEST STRAIGHT TERRACE RUNS (world metres):")
    for k in range(min(a.top, len(runs))):
        ln, kind, fixed, s, e = runs[k]
        if kind == "row":
            x0 = (a.vx0 + s) * VOXEL_MM / 1000.0
            x1 = (a.vx0 + e) * VOXEL_MM / 1000.0
            y = (a.vy0 + fixed) * VOXEL_MM / 1000.0
            print(f"  {k+1}. {ln:4d} voxels = {ln * VOXEL_MM / 1000.0:5.1f} m  "
                  f"EAST-WEST at y={y:.1f}, x from {x0:.1f} to {x1:.1f}")
        else:
            y0 = (a.vy0 + s) * VOXEL_MM / 1000.0
            y1 = (a.vy0 + e) * VOXEL_MM / 1000.0
            x = (a.vx0 + fixed) * VOXEL_MM / 1000.0
            print(f"  {k+1}. {ln:4d} voxels = {ln * VOXEL_MM / 1000.0:5.1f} m  "
                  f"NORTH-SOUTH at x={x:.1f}, y from {y0:.1f} to {y1:.1f}")

    # Render: level bands in alternating shades so the terraces are obvious, the
    # boundary set in dark, and the single longest run in red.
    shade = ((level - level.min()) % 2).astype(np.uint8)
    img = np.where(shade[..., None] == 1, np.array([196, 186, 170], np.uint8),
                   np.array([164, 152, 134], np.uint8))
    img[b] = np.array([70, 62, 52], np.uint8)
    im = Image.fromarray(img, "RGB").resize((a.n * 2, a.n * 2), Image.NEAREST)
    d = ImageDraw.Draw(im)
    for k in range(min(a.top, len(runs))):
        ln, kind, fixed, s, e = runs[k]
        col = (220, 40, 40) if k == 0 else (230, 140, 40)
        w = 5 if k == 0 else 3
        if kind == "row":
            d.line([(s * 2, fixed * 2), (e * 2, fixed * 2)], fill=col, width=w)
        else:
            d.line([(fixed * 2, s * 2), (fixed * 2, e * 2)], fill=col, width=w)
    im.save(a.out)
    print(f"\nwrote {a.out}  ({a.n * VOXEL_MM / 1000.0:.1f} m square, "
          f"alternating shades = adjacent voxel levels, dark = terrace boundary, "
          f"red = the longest single straight run)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
