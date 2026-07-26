#!/usr/bin/env python3
"""Percentage of pixels differing by more than a threshold, for screenshot A/Bs.

The statistic is "% of pixels whose max per-channel absolute difference exceeds
8/255", always quoted against a same-config repeat-run noise floor. Two images
that look identical routinely differ by double digits, and a pooled primitive
that silently drops geometry looks fine at a glance -- so the eye is not
admissible evidence here.
"""
import sys
from PIL import Image, ImageChops


def diff(a_path, b_path, thresh=8):
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        raise SystemExit(f"size mismatch: {a.size} vs {b.size}")
    d = ImageChops.difference(a, b)
    # Max over the three channels, per pixel.
    r, g, bl = d.split()
    m = ImageChops.lighter(ImageChops.lighter(r, g), bl)
    hist = m.histogram()
    total = a.size[0] * a.size[1]
    over = sum(hist[thresh + 1:])
    mean = sum(i * n for i, n in enumerate(hist)) / total
    return 100.0 * over / total, mean, total


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit("usage: imgdiff.py A.png B.png [threshold]")
    t = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    pct, mean, total = diff(sys.argv[1], sys.argv[2], t)
    print(f"{pct:.2f}% of {total} pixels differ by more than {t}/255 "
          f"(mean max-channel delta {mean:.2f}/255)")
