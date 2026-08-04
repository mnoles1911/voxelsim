#!/usr/bin/env python3
"""Percentage of pixels differing by more than a threshold, for screenshot A/Bs.

The statistic is "% of pixels whose max per-channel absolute difference exceeds
8/255", always quoted against a same-config repeat-run noise floor. Two images
that look identical routinely differ by double digits, and a pooled primitive
that silently drops geometry looks fine at a glance -- so the eye is not
admissible evidence here.

WHY THERE IS A --where, AND WHY A CONTROL PAIR IS NOT READABLE WITHOUT IT
------------------------------------------------------------------------
A percentage alone cannot tell the two failure modes of an A/B apart, and that
cost a shipped conclusion. A control pair differed from its partner by 85% of
pixels because the EXPOSURE had moved between the two runs; the number was
reported as though it were the effect of the variable under test, and the
conclusion drawn from it was wrong.

A pair whose difference IS the thing you changed is LOCALISED -- the water, the
crater, the one object. A pair whose difference is an exposure, a sun position
or a stream-in state is DIFFUSE, and covers the sky and the rock as well. Those
two are distinguishable, but only spatially, which is what a single percentage
throws away.

So --where prints three things, to be read together:

  * a coarse grid of per-cell differing-pixel percentage -- the picture of the
    difference, at a glance;
  * CONCENTRATION, the share of all differing pixels falling in the densest 10%
    of cells. 0.10 is perfectly diffuse (the control is broken); near 1.0 is a
    single object (the control is good);
  * the tight box holding 90% of the differing pixels, as a fraction of frame
    area. A localised difference has a small one.

If the diff is large AND diffuse, say the control is broken and fix it. Do not
draw a conclusion from that pair.
"""
import sys
from PIL import Image, ImageChops


def _max_channel_delta(a_path, b_path):
    """The per-pixel max-over-channels absolute difference, as an L image."""
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        raise SystemExit(f"size mismatch: {a.size} vs {b.size}")
    d = ImageChops.difference(a, b)
    # Max over the three channels, per pixel.
    r, g, bl = d.split()
    return a, ImageChops.lighter(ImageChops.lighter(r, g), bl)


def diff(a_path, b_path, thresh=8):
    a, m = _max_channel_delta(a_path, b_path)
    hist = m.histogram()
    total = a.size[0] * a.size[1]
    over = sum(hist[thresh + 1:])
    mean = sum(i * n for i, n in enumerate(hist)) / total
    return 100.0 * over / total, mean, total


def where(a_path, b_path, thresh=8, cols=16, rows=9):
    """WHERE the differing pixels are. See the module docstring for why."""
    import numpy as np

    _a, m = _max_channel_delta(a_path, b_path)
    d = np.asarray(m, dtype=np.int16) > thresh
    H, W = d.shape
    if not d.any():
        print(f"  no pixels differ by more than {thresh}/255 -- the pair is identical.")
        return

    ys = np.linspace(0, H, rows + 1).astype(int)
    xs = np.linspace(0, W, cols + 1).astype(int)
    counts = np.zeros((rows, cols), dtype=float)
    for j in range(rows):
        for i in range(cols):
            counts[j, i] = d[ys[j]:ys[j + 1], xs[i]:xs[i + 1]].sum()
    sizes = np.outer(np.diff(ys), np.diff(xs)).astype(float)

    print(f"  per-cell % differing, {cols}x{rows} grid over the frame:")
    for j in range(rows):
        print("   " + "".join(f"{100.0 * counts[j, i] / sizes[j, i]:6.1f}" for i in range(cols)))

    flat = np.sort(counts.ravel())[::-1]
    top = max(1, int(round(0.10 * flat.size)))
    conc = float(flat[:top].sum()) / float(flat.sum())

    def _span(mass):
        c = mass.cumsum() / mass.sum()
        lo = int((c < 0.05).sum())
        hi = int((c < 0.95).sum())
        return lo, max(hi, lo + 1)

    x_lo, x_hi = _span(d.sum(axis=0).astype(float))
    y_lo, y_hi = _span(d.sum(axis=1).astype(float))
    box = ((x_hi - x_lo) * (y_hi - y_lo)) / float(W * H)

    print(f"  CONCENTRATION {conc:.2f} of differing pixels in the densest {top}/{flat.size} "
          f"cells (0.10 = perfectly diffuse => CONTROL IS BROKEN; near 1.0 = one object)")
    print(f"  90% box x[{x_lo},{x_hi}] y[{y_lo},{y_hi}] = {100.0 * box:.1f}% of frame area")


if __name__ == "__main__":
    argv = [x for x in sys.argv[1:] if not x.startswith("--")]
    flags = {x for x in sys.argv[1:] if x.startswith("--")}
    if len(argv) < 2:
        raise SystemExit("usage: imgdiff.py A.png B.png [threshold] [--where]")
    t = int(argv[2]) if len(argv) > 2 else 8
    pct, mean, total = diff(argv[0], argv[1], t)
    print(f"{pct:.2f}% of {total} pixels differ by more than {t}/255 "
          f"(mean max-channel delta {mean:.2f}/255)")
    if "--where" in flags:
        where(argv[0], argv[1], t)
