"""Does this stone actually have a WAIST, and how deep is it?

A balanced rock, a hoodoo and a sea stack are all the same claim: the stone is
narrower at some height than it is above that height. That claim is invisible
in a voxel count and almost invisible in the isometric preview, where a neck a
metre deep on a thirteen metre boulder is a few pixels of shading. It is
completely obvious in a plot of cross-section against height, which is what
this prints.

Two numbers come out of it. The WAIST is the narrowest level in the middle of
the stone; the OVERHANG is how much wider the widest level above the waist is.
An overhang under about 15% is a rock with a dent in it. A balanced rock is
somewhere past 60%.

    python tools/waistprobe.py hero-balanced-rock
    python tools/waistprobe.py hero-sea-stack --cached
    python tools/waistprobe.py hero-balanced-rock --cm 10

`--cached` reads the grid `tools/archprobe.py` or an earlier run left behind,
because a 34 m stone is minutes to rebuild and the questions come in batches.

`--cm` builds at a coarser lattice. Worth having now and not before: the
weathering budget used to be a fixed number of voxels, so the same spec came
out a DIFFERENT SHAPE at 5 cm and at 10 cm and a coarse preview told you
nothing about the asset. Now that the budget is a fraction of the stone, the
coarse build is the same rock with fewer voxels in it, and tuning a hero at
10 cm costs about a tenth of what tuning it at 5 cm does.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

import _path  # noqa: F401  (sys.path bootstrap)
from forge import pipeline, spec as sm

ROOT = Path(__file__).resolve().parents[1]


def profile(occ: np.ndarray) -> np.ndarray:
    """Solid voxels per horizontal level, over the stone's own height."""
    per_z = occ.sum(axis=(0, 1))
    nz = np.flatnonzero(per_z)
    return per_z[nz[0]:nz[-1] + 1] if nz.size else per_z


def main():
    name = sys.argv[1]
    cm = float(sys.argv[sys.argv.index("--cm") + 1]) if "--cm" in sys.argv else None
    seed = int(sys.argv[sys.argv.index("--seed") + 1]) if "--seed" in sys.argv else 1
    tag = f"{name}" + (f"-{cm:g}cm" if cm else "") + (f"-s{seed}" if seed != 1 else "")
    cache = ROOT / "out" / f"{tag}.npz"
    if "--cached" in sys.argv and cache.exists():
        z = np.load(cache)
        occ = z["data"] != 0
        voxel_m = float(z["voxel_m"])
    else:
        s, _ = sm.load(ROOT / "specs" / f"{name}.json")
        a = pipeline.build(s, seed, resolution_cm=cm)
        occ, voxel_m = a.grid.data != 0, a.grid.voxel_m
        np.savez_compressed(cache, data=a.grid.data, voxel_m=voxel_m)

    # Rubble is scattered around the base as separate lumps and it is not part
    # of the stone's profile -- left in, it fills the bottom of the plot with
    # material that is nowhere near the body and hides the real footing.
    from scipy import ndimage
    whole = occ
    lab, n = ndimage.label(occ, structure=np.ones((3, 3, 3), bool))
    if n > 1:
        occ = lab == (np.bincount(lab.ravel())[1:].argmax() + 1)
        print(f"  {n} separate pieces; the body is "
              f"{100 * occ.sum() / whole.sum():.0f}% of the voxels")

    # Weathering strong enough to cut a notch is strong enough to cut right
    # through, and nothing downstream notices: rocks deliberately skip the
    # orphan sweep so their rubble survives, so a stack sawn off at the tide
    # line comes back as a body hanging in the air above its own debris and
    # every other number about it still looks healthy. Worth one line.
    floor = int(np.nonzero(occ.any(axis=(0, 1)))[0][0])
    if floor > 2:
        print(f"  ** SEVERED: the body starts {floor * voxel_m:.1f} m up and "
              f"stands on nothing. The undercut cut through.")

    per_z = profile(occ).astype(float)
    h = len(per_z)
    # Equivalent radius of each level, which is what "narrower" should mean --
    # area halves for a stone that stays the same shape and loses 29% of its
    # width, and reading area directly makes every waist look twice as deep as
    # it is.
    rad = np.sqrt(per_z / np.pi)

    # The waist is not simply the narrowest level. Every stone tapers to a
    # point at the top, so "narrowest in the middle" reliably picks the summit
    # and reports an overhang of zero on a rock that plainly has a neck -- it
    # did exactly that on the first run. What is being looked for is a PAIR: a
    # level with something wider standing above it. So take the level whose
    # widest neighbour above is the biggest multiple of itself.
    # Above the footing. Every part-buried stone spreads where it enters the
    # ground, so the base-to-belly ratio of a plain egg is around 30% and a
    # search that includes the footing reports that as a neck.
    lo, hi = int(0.15 * h), int(0.97 * h)
    best = (0.0, lo, lo)
    for w in range(lo, hi):
        c = int(np.argmax(rad[w:hi])) + w
        r = rad[c] / max(rad[w], 1e-9)
        if r > best[0]:
            best = (r, w, c)
    _, w, cap = best
    over = (rad[cap] - rad[w]) / max(rad[w], 1e-9)

    print(f"{name}: {h} levels, {h * voxel_m:.1f} m tall, "
          f"{int(occ.sum()):,} voxels")
    print(f"  waist at {w / h:.0%} height ({w * voxel_m:.1f} m up), "
          f"equivalent radius {rad[w] * voxel_m:.2f} m")
    print(f"  widest above it at {cap / h:.0%} ({cap * voxel_m:.1f} m up), "
          f"radius {rad[cap] * voxel_m:.2f} m")
    verdict = ("a neck" if over > 0.60 else
               "a shoulder, not a neck" if over > 0.25 else
               "no undercut; this is a lump")
    print(f"  OVERHANG {100 * over:.0f}%  -- {verdict}")

    step = max(1, h // 34)
    for z in range(0, h, step):
        bar = "#" * int(round(40 * rad[z] / max(rad.max(), 1e-9)))
        mark = "  <- waist" if abs(z - w) < step else ""
        print(f"  {z * voxel_m:6.1f} m {rad[z] * voxel_m:5.2f} |{bar}{mark}")

    # And the silhouette, because the profile is a summary and the shape is the
    # thing being judged. Drawn from EVERYTHING, not from the body alone: on a
    # tor the loose blocks are the asset, and the preview renderer looks down
    # an isometric corner where a squat stack of boulders reads as a pancake.
    # A true side elevation is the only picture that answers "is this a stack".
    for axis, label in ((0, "x"), (1, "y")):
        sil = whole.any(axis=axis)
        img = np.zeros(sil.shape + (3,), np.uint8)
        img[sil] = (210, 200, 180)
        Image.fromarray(np.transpose(img, (1, 0, 2))[::-1]).save(
            ROOT / "out" / f"{name}-profile-{label}.png")
    print(f"  wrote out/{name}-profile-x.png and -y.png")


if __name__ == "__main__":
    main()
