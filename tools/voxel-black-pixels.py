#!/usr/bin/env python3
"""Count near-black pixels below the horizon in moving-capture frames: a void detector.

WHY THIS AND NOT A COUNTER. Every in-engine coverage instrument fails while
moving: the record probe counts records, not pixels, and moves 30% between
identical runs; the ray statistics (HoleStats, either level) collapse streaming
to a fifth of its throughput and blacken the world they are meant to measure.
A frame is the only witness that cannot perturb the flight. A hole in this
renderer is UNLIT -- the marcher drew nothing, so the pixel is the clear colour
-- while water is dark BLUE and shadowed rock is brown; RGB all below the
threshold is a void and nothing else in these frames is.

Reads the region below a horizon fraction (sky excluded) and outside the HUD box.
Prints per-frame void pixels and the per-arm sum, and nothing else: whether a
number is acceptable is the owner's call.
"""
import sys, glob, os, re
from PIL import Image
THRESH = 55          # max(R,G,B) <= this = void. Calibrated 2026-09-13: the square slots the
                     # owner sees read 40-70; clean frames 0.00% at 55; water and shadowed rock never trip it.
HORIZON = 0.40       # fraction of height above which is sky at pitch -10
HUD = (0, 0, 640, 300)  # HUD box in ORIGINAL pixels (2560x1440 frames)

def score(path):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    px = im.load()
    y0 = int(h * HORIZON)
    black = 0
    for y in range(y0, h, 2):
        for x in range(0, w, 2):
            if x < HUD[2] and y < HUD[3]:
                continue
            r, g, b = px[x, y]
            if max(r, g, b) <= THRESH:
                black += 1
    return black * 4, (w * (h - y0))  # sampled every 2nd pixel each axis

def main():
    tags = sys.argv[1:]
    root = r'D:\voxelsim\ue-project\Saved\Screenshots\WindowsEditor'
    for tag in tags:
        files = sorted(glob.glob(os.path.join(root, f'VoxelMove_{tag}-d*_00000.png')))
        total = 0; rows = []
        for f in files:
            m = re.search(r'-d(\d+)_', f); d = int(m.group(1)) if m else -1
            b, area = score(f); total += b
            rows.append(f'{d:5d}m:{100.0*b/area:5.2f}%')
        print(f'{tag:<22} frames={len(files):2d}  void-pixels total={total:9d}   ' + ' '.join(rows))

if __name__ == '__main__':
    main()
