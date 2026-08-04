#!/usr/bin/env python3
"""Mean tone over named rectangles of a capture, for shell-meets-surface joins.

WHAT THIS IS FOR
----------------
The sea is drawn by a 40 km plane and is deliberately NOT meshed; a lake or a
river reach IS meshed. So wherever one meets the other -- a breach in a coast,
a river mouth -- two completely different renderers are drawing water side by
side, and the question "do they match" has to be answered with numbers rather
than with an opinion about a screenshot. The equivalent join for lakes measured
+44.5 against +76.8 on the statistic below.

THE STATISTIC, STATED SO IT CANNOT DRIFT
----------------------------------------
    blueness = B - (R + G) / 2          per pixel, on 0..255 sRGB
    reported as the MEAN over the rectangle, plus the mean R, G, B

It is a difference of channels, so a uniform exposure change moves it far less
than it moves any single channel -- which matters here, because the two regions
being compared are in the SAME frame and must stay comparable. It is not a
perceptual metric and is not claimed to be one. Quote it only against another
number produced the same way.

WHY RECTANGLES AND NOT A SEGMENTATION. A colour-keyed mask of "the water" would
be deciding the answer with the question: the plane and the voxel water are
different colours, which is the thing being measured. A rectangle is a stated,
checkable region -- print it, and anyone can see on the image whether it is
where you said it was.

    imgtone.py shot.png plane:1200,900,1500,1000 voxel:1100,1050,1400,1150

Each region is  label:x0,y0,x1,y1  in pixels, x right and y down from the top
left, x1/y1 exclusive.
"""
import sys

import numpy as np
from PIL import Image


def tone(arr, x0, y0, x1, y1):
    r = arr[y0:y1, x0:x1, 0].astype(np.float64)
    g = arr[y0:y1, x0:x1, 1].astype(np.float64)
    b = arr[y0:y1, x0:x1, 2].astype(np.float64)
    if r.size == 0:
        raise SystemExit(f"empty rectangle {x0},{y0},{x1},{y1}")
    blue = b - 0.5 * (r + g)
    return r.mean(), g.mean(), b.mean(), blue.mean(), blue.std(), r.size


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit("usage: imgtone.py IMAGE.png label:x0,y0,x1,y1 [label:...]")
    img = np.asarray(Image.open(sys.argv[1]).convert("RGB"))
    print(f"{sys.argv[1]}  ({img.shape[1]}x{img.shape[0]})")
    print(f"  {'region':<16}{'px':>9}{'R':>8}{'G':>8}{'B':>8}{'blueness':>10}{'sd':>8}")
    rows = []
    for spec in sys.argv[2:]:
        label, _, box = spec.partition(":")
        x0, y0, x1, y1 = (int(v) for v in box.split(","))
        R, G, B, blue, sd, n = tone(img, x0, y0, x1, y1)
        rows.append((label, blue))
        print(f"  {label:<16}{n:>9}{R:>8.1f}{G:>8.1f}{B:>8.1f}{blue:>10.1f}{sd:>8.1f}")
    if len(rows) == 2:
        (la, ba), (lb, bb) = rows
        print(f"  MISMATCH {lb} - {la} = {bb - ba:+.1f} blueness "
              f"({la} {ba:+.1f}, {lb} {bb:+.1f})")
