"""Pixel-diff a capture against its control, and say whether the control is sound.

WHY THIS EXISTS AS A FILE. A capture A/B is only evidence if the control differs
from its pair in the ONE variable under test and nowhere else, and that has
failed twice on this project from two different causes:

  * a control that differed from its pair by 85% of pixels because auto-exposure
    moved between the two runs -- it proved nothing, and a wrong conclusion was
    drawn from it anyway;
  * a control run whose water was inherited from the previous run, because the
    capture path cleared the `.vxlog` edit log but not the `.vxwater` blob, and
    it restored 886,179,570 fill units before its own dig.

Both are visible in one number if you look at the right one. A sound control for
a THIN feature (a river ribbon a few pixels wide) differs from its pair in a
SMALL, LOCALISED set of pixels. A broken control differs everywhere, faintly --
which is the exposure signature -- or everywhere, strongly.

So this prints both the changed-pixel FRACTION and how CONCENTRATED the change
is, and refuses to give a single "pass" number, because the two failure modes
look different and are not interchangeable.

Usage:
    python tools/capture-pixdiff.py ON.png OFF.png [--thresh 8] [--out DIFF.png]
"""

import argparse
import sys

import numpy as np
from PIL import Image


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("on")
    ap.add_argument("off")
    # 8/255 on any channel. Below this is encoder noise and TAA jitter between
    # two runs of the same deterministic scene; above it is content.
    ap.add_argument("--thresh", type=int, default=8)
    ap.add_argument("--out", default=None, help="write a diff heat image here")
    args = ap.parse_args()

    a = np.asarray(Image.open(args.on).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(args.off).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        print(f"SHAPE MISMATCH {a.shape} vs {b.shape} -- not comparable")
        return 2

    d = np.abs(a - b)
    dmax = d.max(axis=2)
    total = dmax.size
    changed = dmax >= args.thresh
    n = int(changed.sum())
    frac = n / total

    print(f"resolution        : {a.shape[1]} x {a.shape[0]}  ({total:,} px)")
    print(f"changed pixels    : {n:,}  ({frac * 100:.4f}% at threshold {args.thresh}/255)")
    print(f"mean |delta|      : {d.mean():.3f}   max {int(d.max())}")

    # THE EXPOSURE TEST, AND IT MUST BE THE **SIGNED** MEAN.
    #
    # The obvious test is the mean |delta| over the pixels that did NOT change:
    # if the background itself moved, the control is contaminated. That is the
    # right question and the wrong statistic, and it produced a false alarm on
    # the first water-heavy pair it saw -- mean |delta| 2.13, flagged as
    # exposure drift, when the exposure had not moved at all.
    #
    # An exposure or tonemap shift is a BIAS: every pixel goes the same way, so
    # the signed mean is large. Temporal render noise -- TAA jitter, stochastic
    # reflection/SSR sampling, the water surface's own accumulation -- is
    # SYMMETRIC: pixels move both ways, the signed mean collapses to ~0 while
    # the absolute mean stays high. Measured on the pair that raised the false
    # alarm: signed -0.055 against absolute 2.129, a ratio of 0.026.
    #
    # So both are printed, and only the ratio decides. A large absolute mean
    # with a near-zero signed mean is a NOISY comparison, which weakens a small
    # effect but does not invalidate the control; a large signed mean is the
    # thing that actually destroys an A/B.
    bgmask = ~changed
    if bgmask.any():
        sgn = (a - b)[bgmask]
        signed = float(sgn.mean())
        absol = float(np.abs(sgn).mean())
        ratio = abs(signed) / absol if absol > 1e-9 else 0.0
        print(f"background        : signed mean {signed:+.4f}, |mean| {absol:.4f}, "
              f"bias ratio {ratio:.3f}  ({int(bgmask.sum()):,} px)")
        if ratio > 0.30 and absol > 0.5:
            print("  WARNING: the background carries a consistent BIAS. This is the exposure/")
            print("  tonemap-drift signature; the two frames were not graded the same way and")
            print("  the diff below is not attributable to the variable under test.")
        elif absol > 1.5:
            print("  NOTE: the background is noisy but UNBIASED -- temporal render noise, not")
            print("  exposure. The control is usable; small effects are just harder to see")
            print("  against it. Raise the threshold rather than discarding the pair.")

    if n == 0:
        print("\nNO DIFFERENCE AT ALL. Either the switch did nothing, or the feature was")
        print("off-screen/occluded in both frames. This is not evidence that it works.")
        return 0

    # THE THRESHOLD LADDER, AND IT IS THE POINT OF THE WHOLE SCRIPT. A single
    # threshold cannot separate "a thin feature appeared" from "the frame is
    # full of temporal dither", because at a LOW threshold both look like
    # scattered pixels across the whole image -- the first run of this printed a
    # bbox covering 99.84% of the frame for a river that occupies a few hundred
    # pixels, which reads as a broken control and was not one.
    #
    # Raising the threshold separates them, because they have different
    # amplitudes: TAA/encoder jitter between two runs of the same deterministic
    # scene is a few levels, and a feature that is actually drawn is tens to
    # hundreds. If the changed set SHRINKS and its bbox CONTRACTS as the
    # threshold rises, the difference is a localised feature. If it stays spread
    # out, it is a global shift and the control is broken.
    print("\nthreshold ladder  -- a real feature contracts, a global shift does not:")
    print(f"    {'thr':>4} {'changed px':>12} {'%frame':>8}  {'bbox (x0..x1, y0..y1)':<28} {'bbox %':>7}")
    for t in (args.thresh, 16, 32, 64, 96, 128):
        m = dmax >= t
        c = int(m.sum())
        if c == 0:
            print(f"    {t:>4} {0:>12} {0.0:>8.4f}  {'-':<28} {'-':>7}")
            continue
        yy, xx = np.nonzero(m)
        bb = (int(yy.max() - yy.min() + 1)) * (int(xx.max() - xx.min() + 1))
        box = f"{xx.min()}..{xx.max()}, {yy.min()}..{yy.max()}"
        print(f"    {t:>4} {c:>12,} {c / total * 100:>8.4f}  {box:<28} {bb / total * 100:>6.1f}%")

    if frac > 0.25:
        print("  WARNING: over a quarter of the frame changed at the base threshold. For a")
        print("  thin feature that is a broken control, not a strong result -- check exposure")
        print("  and persisted state.")

    if args.out:
        heat = np.zeros(a.shape[:2] + (3,), dtype=np.uint8)
        heat[..., 0] = np.clip(dmax * 4, 0, 255)
        heat[changed, 1] = 255
        Image.fromarray(heat).save(args.out)
        print(f"wrote             : {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
