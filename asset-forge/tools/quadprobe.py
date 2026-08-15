"""Does each land-animal slider actually do anything? Measure it and say so.

THE SIGNATURE FAILURE OF THIS PROJECT IS THE SILENT NO-OP. A weathering pass
removed 20 voxels out of 90,000 for months while reporting success. A
`fish.length_m` ceiling of 3 m clamped every whale at authoring time. A
`bill_gape` was multiplied by `bill_depth` so it did nothing below heron size.
Both of a raven's wings shipped under one part id, which no render would ever
have shown. Every one of those ran, cost time, and changed nothing.

So every mechanism in `forge/quadruped.py` gets a number here that moves when
its slider moves, and this tool prints the number and says DEAD when it does
not.

    python tools/quadprobe.py                 # sweep every quadruped parameter
    python tools/quadprobe.py --seeds 6       # more seeds, tighter answer
    python tools/quadprobe.py --stance        # do all four feet reach the floor?
    python tools/quadprobe.py --parts         # are four legs FOUR parts?
    python tools/quadprobe.py --caps          # joint balls: drawn, or skipped?
    python tools/quadprobe.py --lattice       # each species one tier finer
    python tools/quadprobe.py --read          # can the markings be seen?
    python tools/quadprobe.py --sex           # male against female
    python tools/quadprobe.py --all           # everything

WHY IT AVERAGES OVER SEEDS. Changing any parameter changes the seed hash, and
`pipeline.rng_for` mixes the hash into the seed -- so a one-seed A/B is not the
same animal twice, it is two different animals. Four seeds is enough to see a
real effect through that; one is enough to invent one. The sweep also pins
`variation.amount` to zero, because every land-animal spec authors a 9% length
variation and a muzzle is eight voxels long.

WHAT "MOVES" MEANS HERE. A parameter passes if its measurement changes by more
than one voxel AND by more than 4% across its authored range. Both, because
either alone lies: a percentage is meaningless on a quantity that is 2 voxels,
and one voxel of change on a 90-voxel measurement is rounding.

WHERE THIS DIFFERS FROM `tools/birdprobe.py`, AND IT IS ONE THING. The bird
probe reads the MATERIAL HISTOGRAM to find a part, because a bird's tag grid
was scratch and never left the generator. It does not any more --
`pipeline.build` carries `asset.parts` out for the rig -- so most measurements
here read the PART TAGS instead. That is strictly better for geometry: it does
not need a species to be painted in a colour nothing else uses, and it cannot
be fooled by two parts sharing a material.

It is strictly WORSE for anything about colour, and the four measurements at
the bottom of the table read materials for exactly that reason. A tag-based
measurement of a marking would prove that the generator knows where the
marking is, which is not the question. `bird._paint` had a version that knew
where every wing bar went and painted none of them.

FOUR SEPARATE CHECKS BEYOND THE SWEEP, and each exists because the sweep cannot
see the thing it is looking for:

* `--stance` measures each of the four feet against the lowest voxel of the
  asset. `pipeline.build` CROPS the grid, so its "floating" health check is
  answered in the affirmative for every quadruped whatever the feet did; this
  is the only thing that can tell a standing animal from one on tiptoe.
* `--parts` is the raven's-wings check. Four legs must be four ids with four
  disjoint sets of voxels and four joints. Nothing renders wrong when they are
  not, which is precisely why it needs measuring.
* `--caps` says whether each joint's cap sphere was DRAWN or SKIPPED and what
  limb thickness decided it, because "skipped below three voxels" is a rule
  that is invisible either way in a render.
* `--lattice` builds each species one tier finer and prints both, which is how
  the choice recorded in each spec's notes gets checked rather than believed.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

import _path  # noqa: F401  (sys.path bootstrap)
from forge import materials, parts as partslib, pipeline, quadruped as _q
from forge import spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"

# The four leg ids, after `parts.split_sides`. Written out rather than derived,
# because the point of this file is to check the derivation.
LEG_FL = partslib.P_LEG
LEG_FR = partslib.P_LEG + partslib.SIDE_STRIDE
LEG_HL = partslib.P_LEG + partslib.AXIS_STRIDE
LEG_HR = partslib.P_LEG + partslib.AXIS_STRIDE + partslib.SIDE_STRIDE
LEGS = (LEG_FL, LEG_FR, LEG_HL, LEG_HR)


# --- measurements -----------------------------------------------------------
#
# Each of these is a number a HUMAN could check off a render, which is the
# point: a probe that measures an internal variable proves the variable exists,
# not that it reached the voxels.


def _occ(a):
    return a.grid.data != 0


def _part(a, pid) -> np.ndarray:
    """Voxels belonging to one shared part id, or to any of several."""
    if a.parts is None:
        return np.zeros(a.grid.data.shape, bool)
    if isinstance(pid, (tuple, list, set, frozenset)):
        m = np.zeros(a.parts.shape, bool)
        for q in pid:
            m |= a.parts == q
        return m
    return a.parts == pid


# ABSENT IS ZERO, NOT UNKNOWN, and the distinction cost six ERROR rows on the
# first run of this file. A measurement of a part that CAN be authored away --
# a muzzle at share 0, a tail at length 0, an ear or a horn set to "none" -- has
# a right answer at the bottom of its slider, and that answer is 0. Returning
# NaN there makes `_verdict` report ERROR, which reads as "the probe is broken"
# rather than "the low end of this slider removes the part", and six working
# sliders were flagged that way. `_absent` marks the measurements where nothing
# is a legitimate outcome; every other one still returns NaN, because a NaN out
# of `m_back_slope` really would mean the probe could not find the animal.
def _absent(fn):
    def wrapped(a):
        v = fn(a)
        return 0.0 if (v is None or np.isnan(v)) else v
    wrapped.__name__ = fn.__name__
    wrapped.__doc__ = fn.__doc__
    return wrapped


def _bbox(m):
    """(x0, x1, y0, y1, z0, z1) of a mask, or None if it is empty."""
    if not m.any():
        return None
    xs, ys, zs = np.nonzero(m)
    return (int(xs.min()), int(xs.max()), int(ys.min()), int(ys.max()),
            int(zs.min()), int(zs.max()))


def _cent(m):
    if not m.any():
        return None
    xs, ys, zs = np.nonzero(m)
    return float(xs.mean()), float(ys.mean()), float(zs.mean())


def _nan(v):
    return float("nan") if v is None else float(v)


def m_length(a) -> float:
    """Columns the animal occupies front to back."""
    return float(_occ(a).any(axis=(1, 2)).sum())


def m_height(a) -> float:
    return float(_occ(a).any(axis=(0, 1)).sum())


def m_width(a) -> float:
    return float(_occ(a).any(axis=(0, 2)).sum())


def m_voxels(a) -> float:
    return float(a.stats["voxels"])


def _body(a):
    return _part(a, partslib.P_BODY)


def m_belly_clear(a) -> float:
    """HOW HIGH THE ANIMAL STANDS: the gap between the bottom of the trunk and
    the ground, in voxels.

    This is the measurement for `quad.shoulder_h`, and it is deliberately not
    "how tall is the animal". Total height moves when the neck angle, the ears
    or a rack of antlers move, so a height measurement would report the shoulder
    slider as working on a spec where it had been disconnected and the head had
    simply gone up. Belly clearance is the leg length and nothing else.
    """
    b = _bbox(_body(a))
    if b is None:
        return float("nan")
    return float(b[4] - _bbox(_occ(a))[4])


def _back_top(a, lo: float, hi: float) -> float:
    """Top of the trunk, averaged over the columns between two fractions of its
    own length."""
    m = _body(a)
    bb = _bbox(m)
    if bb is None:
        return float("nan")
    x0, x1 = bb[0], bb[1]
    span = max(x1 - x0, 1)
    a0 = int(round(x0 + lo * span))
    a1 = max(a0 + 1, int(round(x0 + hi * span)))
    sl = m[a0:a1]
    if not sl.any():
        return float("nan")
    tops = []
    for i in range(sl.shape[0]):
        col = sl[i].any(axis=0)
        if col.any():
            tops.append(float(np.nonzero(col)[0].max()))
    return float(np.mean(tops)) if tops else float("nan")


def m_back_slope(a) -> float:
    """HOW FAR THE SHOULDER STANDS ABOVE THE HIP, in voxels of the back's own
    top line. This is the measurement for `quad.hip_h`.

    Taken as the top of the trunk rather than as the joint positions, because
    the top of the back is what a person looking at a bison actually sees, and
    because a joint position is an internal variable -- the thing this file
    exists not to measure. Positive means shoulders higher.
    """
    return _back_top(a, 0.70, 0.95) - _back_top(a, 0.05, 0.30)


def m_trunk_run(a) -> float:
    b = _bbox(_body(a))
    return float("nan") if b is None else float(b[1] - b[0] + 1)


def m_head_fwd(a) -> float:
    """How far the head's centre is forward of the trunk's front end.

    The measurement for `quad.neck_frac`, and it is a horizontal one on purpose:
    a neck lengthened on a steeply-angled species goes mostly UP, so a
    height-based measurement would call the slider dead on a giraffe and alive
    on a stoat. Forward reach moves on both, because the head leaves the withers
    at an angle that is never vertical.
    """
    h = _cent(_part(a, partslib.P_HEAD))
    b = _bbox(_body(a))
    if h is None or b is None:
        return float("nan")
    return float(h[0] - b[1])


def m_head_rise(a) -> float:
    """How far the head's centre is above the top of the withers. The
    measurement for `quad.neck_deg`."""
    h = _cent(_part(a, partslib.P_HEAD))
    if h is None:
        return float("nan")
    return float(h[2] - _back_top(a, 0.70, 0.95))


def m_head_diam(a) -> float:
    b = _bbox(_part(a, partslib.P_HEAD))
    return float("nan") if b is None else float(b[1] - b[0] + 1)


def m_muzzle_run(a) -> float:
    b = _bbox(_part(a, partslib.P_JAW))
    return float("nan") if b is None else float(b[1] - b[0] + 1)


def m_muzzle_depth(a) -> float:
    b = _bbox(_part(a, partslib.P_JAW))
    return float("nan") if b is None else float(b[5] - b[4] + 1)


def m_muzzle_width(a) -> float:
    b = _bbox(_part(a, partslib.P_JAW))
    return float("nan") if b is None else float(b[3] - b[2] + 1)


def m_muzzle_drop(a) -> float:
    """How far the nose sits below the back of the muzzle.

    A BEND, NOT A ROTATION, and this measurement is written to tell the two
    apart -- which is the distinction `bird.bill_curve` got wrong for several
    passes by rotating the whole bill and moving only the tip. So it compares
    the muzzle's own front to its own back rather than to the head: a rotation
    moves both together and reports zero here, a bend does not.
    """
    m = _part(a, partslib.P_JAW)
    b = _bbox(m)
    if b is None or b[1] - b[0] < 3:
        return float("nan")
    x0, x1 = b[0], b[1]
    third = max(1, (x1 - x0) // 3)
    front = m[x1 - third:x1 + 1]
    back = m[x0:x0 + third]
    if not front.any() or not back.any():
        return float("nan")
    return float(np.nonzero(back.any(axis=(0, 1)))[0].mean()
                 - np.nonzero(front.any(axis=(0, 1)))[0].mean())


def m_muzzle_tip_h(a) -> float:
    """Height of the nose above the centre of the skull. The measurement for
    `quad.head_deg`.

    Its first version was `m_muzzle_run`, on the reasoning that a muzzle pointed
    down covers fewer columns -- which is true, and is a PROJECTION ARTIFACT
    rather than the thing the slider does. A measurement that moves for the
    wrong reason passes for a working parameter and would go on passing after
    the parameter was disconnected, which is the failure this file is against.
    """
    m = _part(a, partslib.P_JAW)
    b = _bbox(m)
    h = _cent(_part(a, partslib.P_HEAD))
    if b is None or h is None:
        return 0.0
    tip = m[b[1] - max(0, (b[1] - b[0]) // 6):b[1] + 1]
    if not tip.any():
        return 0.0
    return float(np.nonzero(tip.any(axis=(0, 1)))[0].mean() - h[2])


def _depth_at(a, frac: float) -> float:
    """Depth of the trunk at a fraction along its own length, 0 at the rump."""
    m = _body(a)
    b = _bbox(m)
    if b is None:
        return float("nan")
    x = int(round(b[0] + frac * (b[1] - b[0])))
    x = int(np.clip(x, b[0], b[1]))
    col = m[x].any(axis=0)
    return float(col.sum()) if col.any() else float("nan")


def m_body_depth(a) -> float:
    return _depth_at(a, 0.5)


def m_chest_depth(a) -> float:
    return _depth_at(a, 0.85)


def m_waist_depth(a) -> float:
    return _depth_at(a, 0.5)


def m_rump_depth(a) -> float:
    return _depth_at(a, 0.15)


def m_body_width(a) -> float:
    m = _body(a)
    b = _bbox(m)
    if b is None:
        return float("nan")
    x = (b[0] + b[1]) // 2
    col = m[x].any(axis=1)
    return float(col.sum()) if col.any() else float("nan")


def m_hump(a) -> float:
    """How far the highest point of the back stands above the back's own mean
    height. The measurement for `quad.hump`, and it is a RELATIVE one because a
    hump added to a bigger animal must not read as a bigger hump."""
    m = _body(a)
    b = _bbox(m)
    if b is None:
        return float("nan")
    tops = []
    for x in range(b[0], b[1] + 1):
        col = m[x].any(axis=0)
        if col.any():
            tops.append(float(np.nonzero(col)[0].max()))
    if len(tops) < 4:
        return float("nan")
    return float(max(tops) - np.mean(tops))


def m_hump_at(a) -> float:
    """Where along the back the highest point is, 0 at the rump and 1 at the
    shoulder. The measurement for `quad.hump_at`, in fractions rather than
    voxels because it is a position on a body whose length varies."""
    m = _body(a)
    b = _bbox(m)
    if b is None:
        return float("nan")
    best, at = -1.0, 0.0
    for x in range(b[0], b[1] + 1):
        col = m[x].any(axis=0)
        if col.any():
            t = float(np.nonzero(col)[0].max())
            if t > best:
                best, at = t, x
    return float((at - b[0]) / max(b[1] - b[0], 1))


def m_belly_share(a) -> float:
    """Share of the trunk's voxels above the midline of its own outline. The
    measurement for `quad.belly`."""
    m = _body(a)
    b = _bbox(m)
    if b is None:
        return float("nan")
    mid = 0.5 * (b[4] + b[5])
    zs = np.nonzero(m)[2]
    return float((zs > mid).mean())


def m_neck_vox(a) -> float:
    return float(_part(a, partslib.P_NECK).sum())


def m_neck_thick(a) -> float:
    """Thickness of the neck at its middle, measured across the neck rather
    than along the grid: the widest run of neck voxels in any single column."""
    m = _part(a, partslib.P_NECK)
    b = _bbox(m)
    if b is None:
        return float("nan")
    runs = [float(m[x].sum()) for x in range(b[0], b[1] + 1) if m[x].any()]
    return float(np.median(runs)) if runs else float("nan")


def m_mane_vox(a) -> float:
    return float(_part(a, partslib.P_CREST).sum())


def m_dewlap(a) -> float:
    """How far the neck hangs below its own middle. The measurement for
    `quad.dewlap`, and it reads the NECK part because that is what a bell is
    tagged as -- it rotates with the neck."""
    m = _part(a, partslib.P_NECK)
    b = _bbox(m)
    if b is None:
        return float("nan")
    return float(b[5] - b[4] + 1)


def m_eye(a) -> float:
    """Voxels wearing the eye colour, and only where nothing else wears it.

    `bird._eye`'s probe had to check the eye's colour was not shared with the
    bill or the head before counting, and the same applies here for the same
    reason: a species whose eye and whose mane are both `skin_dark` would report
    an eye of two hundred voxels.
    """
    eye = materials.resolve(sm.get(a.spec, "materials.quad_eye"))
    others = {materials.resolve(sm.get(a.spec, f"materials.quad_{s}"))
              for s in ("back", "belly", "head", "leg", "tail", "mark", "horn")}
    if eye in others:
        return float("nan")
    return float(a.stats["by_material"].get(eye, 0))


def _ear(a):
    return _part(a, (partslib.P_EAR, partslib.P_EAR + partslib.SIDE_STRIDE))


def m_ear_len(a) -> float:
    """How far the ears reach from the centre of the skull."""
    m = _ear(a)
    h = _cent(_part(a, partslib.P_HEAD))
    if not m.any() or h is None:
        return float("nan")
    xs, ys, zs = np.nonzero(m)
    d = np.sqrt((xs - h[0]) ** 2 + (ys - h[1]) ** 2 + (zs - h[2]) ** 2)
    return float(d.max())


def m_ear_face(a) -> float:
    """The ear's own OUTLINE seen from the side, in columns.

    THE MEASUREMENT THAT CAUGHT THE FIRST VERSION OF THE EARS. Drawn as a chain
    of balls along their length -- which is what `pointed` correctly does -- a
    blade ear is a tapering CYLINDER, and every length and width measurement
    reported it as a working ear. This one asks how many columns of the side
    view the ear covers, which is what a person looking at a hare sees, and it
    is the only one of the four that separates a paddle from a rod.
    """
    m = _ear(a)
    if not m.any():
        return float("nan")
    return float(m.any(axis=(1, 2)).sum())


def m_ear_span(a) -> float:
    """How far apart the two ears are held, in voxels. The measurement for
    `quad.ear_deg`: at 90 degrees they stand straight up and nearly touch, at 0
    they lie out sideways."""
    m = _ear(a)
    b = _bbox(m)
    return float("nan") if b is None else float(b[3] - b[2] + 1)


def m_ear_back(a) -> float:
    """How far behind the skull's centre the ears sit."""
    m = _ear(a)
    h = _cent(_part(a, partslib.P_HEAD))
    c = _cent(m)
    if c is None or h is None:
        return float("nan")
    return float(h[0] - c[0])


def _horn(a):
    return _part(a, (partslib.P_HORN, partslib.P_HORN + partslib.SIDE_STRIDE))


def m_horn_len(a) -> float:
    m = _horn(a)
    h = _cent(_part(a, partslib.P_HEAD))
    if not m.any() or h is None:
        return float("nan")
    xs, ys, zs = np.nonzero(m)
    d = np.sqrt((xs - h[0]) ** 2 + (ys - h[1]) ** 2 + (zs - h[2]) ** 2)
    return float(d.max())


def m_horn_vox(a) -> float:
    m = _horn(a)
    return float(m.sum()) if m.any() else float("nan")


def m_horn_span(a) -> float:
    b = _bbox(_horn(a))
    return float("nan") if b is None else float(b[3] - b[2] + 1)


def m_horn_back(a) -> float:
    """How far back the headgear is carried, in columns. The measurement for
    `quad.horn_curl`: a straight spear stands over the skull and a ram's curl
    comes right back down behind the eye."""
    m = _horn(a)
    h = _cent(_part(a, partslib.P_HEAD))
    if not m.any() or h is None:
        return float("nan")
    xs = np.nonzero(m)[0]
    return float(h[0] - xs.min())


def m_horn_tines(a) -> float:
    """How many separate runs of headgear cross a horizontal line two thirds of
    the way up the rack. This is the measurement for `quad.horn_tines`, and it
    is the same trick `birdprobe.m_wing_gaps` uses on a slotted wingtip: a tine
    count is not a size, it is a count of separated things, and only a crossing
    count can see one."""
    m = _horn(a)
    b = _bbox(m)
    if b is None or b[5] - b[4] < 4:
        return 0.0
    # SCANNED AT SEVERAL HEIGHTS AND THE MOST CROSSINGS WINS. Taken at one
    # height -- two thirds up, which is where a rack looks widest -- a
    # nine-point rack and a one-point rack both measured 1 or 2, because tines
    # leave the beam at different heights by design and any single line catches
    # two of them. A points count is the number of separated things ANYWHERE on
    # the rack, so the scan has to look everywhere.
    best = 0
    for f in np.linspace(0.25, 0.92, 12):
        z = int(b[4] + f * (b[5] - b[4]))
        plane = m[:, :, z].any(axis=1)
        if not plane.any():
            continue
        runs = int(np.count_nonzero(np.diff(plane.astype(np.int8)) == 1)
                   + (1 if plane[0] else 0))
        best = max(best, runs)
    return float(best)


def _tail(a):
    return _part(a, partslib.P_TAIL)


def m_tail_len(a) -> float:
    """How far the tail reaches from where it leaves the trunk."""
    m = _tail(a)
    b = _bbox(_body(a))
    if not m.any() or b is None:
        return float("nan")
    xs, ys, zs = np.nonzero(m)
    ref = np.array([b[0], 0.5 * (b[2] + b[3]), 0.5 * (b[4] + b[5])])
    d = np.sqrt((xs - ref[0]) ** 2 + (ys - ref[1]) ** 2 + (zs - ref[2]) ** 2)
    return float(d.max())


def m_tail_vox(a) -> float:
    m = _tail(a)
    return float(m.sum()) if m.any() else float("nan")


def m_tail_base(a) -> float:
    """Thickness of the tail where it leaves the body, in voxels. Measured a
    fifth of the way along rather than at the very base, where the trunk has
    reclaimed the tail's own voxels and the count is the trunk's."""
    m = _tail(a)
    b = _bbox(m)
    if b is None or b[1] - b[0] < 3:
        return float("nan")
    x = int(b[1] - 0.20 * (b[1] - b[0]))
    return float(m[x].sum()) if m[x].any() else float("nan")


def m_tail_tip_h(a) -> float:
    """How close the far end of the tail gets to the ground, in voxels. The
    measurement for `quad.tail_deg`, and the one `--stance` reads to check a
    kangaroo's tripod actually reaches the floor.

    TWO THINGS ABOUT THIS ARE ON THEIR SECOND VERSION AND BOTH LIED THE SAME
    WAY -- they reported a tail that was touching the ground as three voxels
    clear of it, which failed all three bipeds in the library on a mechanism
    that was working.

    The first was taking the MEAN height of the tip voxels. A tail is a rod with
    a thickness, so a rod lying against the floor has its centreline a radius up
    and its mean higher still; what the question asks is whether any of it
    reaches, which is the minimum.

    The second was picking the tip by its x extreme. That is the tip only for a
    tail carried backwards -- a squirrel's tail arches over its own back and a
    warthog's stands straight up, and for both of those the far end in x is
    somewhere in the middle of the tail. Distance from the trunk finds the tip
    whatever the carriage, which is the same correction `m_tuft` needed.
    """
    m = _tail(a)
    b = _bbox(_body(a))
    if not m.any() or b is None:
        return float("nan")
    xs, ys, zs = np.nonzero(m)
    ref = np.array([b[0], 0.5 * (b[2] + b[3]), 0.5 * (b[4] + b[5])])
    d = np.sqrt((xs - ref[0]) ** 2 + (ys - ref[1]) ** 2 + (zs - ref[2]) ** 2)
    far = d >= d.max() - max(1.5, 0.15 * d.max())
    if not far.any():
        return float("nan")
    return float(zs[far].min() - _bbox(_occ(a))[4])


def m_tail_rise(a) -> float:
    """How far the tail's midpoint stands above the straight line between its
    base and its tip. The measurement for `quad.tail_arc`, and it is written
    this way for the same reason `m_muzzle_drop` is: an arc is a departure from
    a straight line, and comparing the tip to the base measures the carriage
    ANGLE instead and reports a working arc slider as dead."""
    m = _tail(a)
    b = _bbox(m)
    if b is None or b[1] - b[0] < 5:
        return float("nan")
    def zat(x):
        col = m[x].any(axis=0)
        return float(np.nonzero(col)[0].mean()) if col.any() else float("nan")
    z0, z1 = zat(b[0]), zat(b[1])
    zm = zat((b[0] + b[1]) // 2)
    if any(np.isnan(v) for v in (z0, z1, zm)):
        return float("nan")
    return float(zm - 0.5 * (z0 + z1))


def m_tuft(a) -> float:
    """How much tail there is at the far END of it, in voxels.

    MEASURED AS A BALL ROUND THE TIP, not as a slice of the grid, and the first
    version was the slice. A zebra carries its tail at -55 degrees, so a single
    x-plane at the tail's furthest column contains a long vertical run of it:
    the measurement returned 673 voxels at both ends of `quad.tail_taper` and
    reported a slider that plainly works as DEAD. A grid-aligned slice measures
    a tail correctly only when the tail lies along a grid axis, which across
    this library's twenty-four species it does on none of them.

    So the tip is found geometrically -- the tail voxel furthest from the trunk
    -- and this counts what is near it. That is carriage-independent, which is
    the whole requirement.
    """
    m = _tail(a)
    b = _bbox(_body(a))
    if not m.any() or b is None:
        return 0.0
    xs, ys, zs = np.nonzero(m)
    ref = np.array([b[0], 0.5 * (b[2] + b[3]), 0.5 * (b[4] + b[5])])
    d = np.sqrt((xs - ref[0]) ** 2 + (ys - ref[1]) ** 2 + (zs - ref[2]) ** 2)
    i = int(np.argmax(d))
    r = max(1.5, 0.10 * float(d[i]))
    near = ((xs - xs[i]) ** 2 + (ys - ys[i]) ** 2 + (zs - zs[i]) ** 2) <= r * r
    return float(near.sum())


def _legs(a):
    return _part(a, LEGS)


def m_leg_vox(a) -> float:
    m = _legs(a)
    return float(m.sum()) if m.any() else float("nan")


def m_leg_thick(a) -> float:
    """Median cross-section of a single foreleg through its own columns."""
    m = _part(a, LEG_FL)
    b = _bbox(m)
    if b is None:
        return float("nan")
    runs = [float(m[:, :, z].sum()) for z in range(b[4], b[5] + 1)
            if m[:, :, z].any()]
    return float(np.median(runs)) if runs else float("nan")


def m_fore_gap(a) -> float:
    """HOW FAR THE FORE FOOT IS OFF THE GROUND, in voxels.

    The measurement for `quad.fore_reach`, and the reason that parameter is not
    gated on the stance. It is also the check `pipeline.health` cannot do: that
    function's floating test reads `ground_contact`, which is taken AFTER
    `pipeline.build` crops the grid, so the bottom slab is occupied by
    definition and every quadruped passes it whatever its legs did.
    """
    m = _part(a, (LEG_FL, LEG_FR))
    if not m.any():
        return float("nan")
    zs = np.nonzero(m)[2]
    return float(zs.min() - _bbox(_occ(a))[4])


def m_hind_gap(a) -> float:
    m = _part(a, (LEG_HL, LEG_HR))
    if not m.any():
        return float("nan")
    zs = np.nonzero(m)[2]
    return float(zs.min() - _bbox(_occ(a))[4])


def m_foot_span(a) -> float:
    """Distance from the front of the fore feet to the back of the hind feet,
    measured in the bottom fifth of the animal. The measurement for
    `quad.hock` and `quad.fore_bend`: folding a limb moves the foot along the
    ground and nothing else does."""
    m = _legs(a)
    bb = _bbox(_occ(a))
    if not m.any() or bb is None:
        return float("nan")
    lo = bb[4]
    band = m[:, :, lo:lo + max(2, (bb[5] - lo) // 8)]
    if not band.any():
        return float("nan")
    xs = np.nonzero(band)[0]
    return float(xs.max() - xs.min() + 1)


def m_hind_foot(a) -> float:
    """Length of the hind foot along the ground, in columns."""
    m = _part(a, (LEG_HL, LEG_HR))
    bb = _bbox(_occ(a))
    if not m.any() or bb is None:
        return float("nan")
    lo = bb[4]
    band = m[:, :, lo:lo + 2]
    if not band.any():
        return float("nan")
    xs = np.nonzero(band)[0]
    return float(xs.max() - xs.min() + 1)


# --- the four that read COLOUR, and why ------------------------------------
#
# See the module docstring: a tag-based measurement of a marking proves the
# generator knows where the marking is, which is not the question anyone is
# asking. `bird._paint` had a version that knew where every wing bar went and
# painted none of them.


def _share(a, mat: int) -> float:
    return 100.0 * a.stats["by_material"].get(mat, 0) / max(a.stats["voxels"], 1)


def _mark_mat(a) -> int:
    return materials.resolve(sm.get(a.spec, "materials.quad_mark"))


def m_mark_pct(a) -> float:
    """Share of the animal wearing the marking colour."""
    return _share(a, _mark_mat(a))


def m_under_pct(a) -> float:
    return _share(a, materials.resolve(sm.get(a.spec, "materials.quad_belly")))


def m_mark_runs(a) -> float:
    """How many separate BANDS of marking cross the animal's back.

    A count, not an area: `quad.mark_count` changes how many stripes there are
    and not how much of the animal is striped, so an area measurement reports
    it dead. This is the same measurement `birdprobe.m_mark_runs` makes and it
    carries the same finding -- above about five bands on a twenty-voxel body
    they merge, so the count SATURATES rather than dying, and the sweep runs it
    on a long species for that reason.
    """
    mat = _mark_mat(a)
    if mat in {materials.resolve(sm.get(a.spec, f"materials.quad_{s}"))
               for s in ("back", "head", "leg", "tail")}:
        return float("nan")
    m = _body(a) & (a.grid.data == mat)
    b = _bbox(_body(a))
    if not m.any() or b is None:
        return 0.0
    # COUNTED ALONG ONE LINE, NOT DOWN A WHOLE COLUMN. Written as
    # `m.any(axis=(1, 2))` -- ask each x column whether it holds any marking --
    # a zebra with forty stripes measured 2.7 bands and `quad.mark_count`
    # reported "weak". The reason is that the bands are phased on distance along
    # the TRUNK'S OWN AXIS, which is tilted, so a single x column crosses a
    # stripe near the belly and a gap near the spine. Every column therefore
    # holds some marking and the run count collapses to one.
    #
    # A person counting a zebra's stripes runs their eye along the flank at one
    # height, and so does this.
    z = int(0.5 * (b[4] + b[5]))
    y = int(0.5 * (b[2] + b[3]))
    best = 0
    for dz in (-2, 0, 2):
        for dy in (0, 2, -2):
            zz = int(np.clip(z + dz, b[4], b[5]))
            yy = int(np.clip(y + dy, b[2], b[3]))
            line = m[:, yy, zz]
            if not line.any():
                continue
            runs = int(np.count_nonzero(np.diff(line.astype(np.int8)) == 1)
                       + (1 if line[0] else 0))
            best = max(best, runs)
    return float(best)


def m_stocking(a) -> float:
    """How far up the legs the marking colour reaches, in voxels off the
    ground."""
    mat = _mark_mat(a)
    m = _legs(a) & (a.grid.data == mat)
    bb = _bbox(_occ(a))
    if not m.any() or bb is None:
        return 0.0
    return float(np.nonzero(m)[2].max() - bb[4])


def m_tail_tip_pct(a) -> float:
    """Share of the tail wearing the marking colour."""
    t = _tail(a)
    if not t.any():
        return float("nan")
    return 100.0 * float((t & (a.grid.data == _mark_mat(a))).sum() / t.sum())


def m_cape_pct(a) -> float:
    """Share of the TRUNK wearing the marking colour, measured on the trunk
    alone -- a cape that also covered the legs would read the same as a longer
    cape if the whole animal were the denominator."""
    b = _body(a)
    if not b.any():
        return float("nan")
    return 100.0 * float((b & (a.grid.data == _mark_mat(a))).sum() / b.sum())


# parameter -> (measurement, unit, label). Swept low against high.
# The measurements whose bottom end is a part that is simply not there. See
# `_absent` for why NaN was the wrong answer and 0 is the right one.
for _fn in ("m_muzzle_run", "m_muzzle_depth", "m_muzzle_width", "m_neck_vox",
            "m_neck_thick", "m_mane_vox", "m_ear_len", "m_ear_face",
            "m_ear_span", "m_horn_len", "m_horn_vox", "m_horn_span",
            "m_horn_back", "m_tail_len", "m_tail_vox", "m_tail_base",
            "m_leg_vox", "m_leg_thick", "m_dewlap"):
    globals()[_fn] = _absent(globals()[_fn])
del _fn

SWEEPS = [
    ("quad.length_m", m_length, "vox", "length, columns"),
    ("quad.shoulder_h", m_belly_clear, "vox", "belly clear of the ground, vox"),
    ("quad.hip_h", m_back_slope, "vox", "shoulder above hip, voxels"),
    ("quad.trunk_frac", m_trunk_run, "vox", "trunk, columns"),
    ("quad.neck_frac", m_head_fwd, "vox", "head forward of the trunk, vox"),
    ("quad.head_frac", m_head_diam, "vox", "skull, columns"),
    ("quad.muzzle_frac", m_muzzle_run, "vox", "muzzle, columns"),
    ("quad.neck_deg", m_head_rise, "vox", "head above the withers, voxels"),
    ("quad.head_deg", m_muzzle_tip_h, "vox", "nose above the skull centre, vox"),
    ("quad.depth", m_body_depth, "vox", "trunk depth, voxels"),
    ("quad.width", m_body_width, "vox", "trunk width, voxels"),
    ("quad.chest", m_chest_depth, "vox", "depth at the shoulder, voxels"),
    ("quad.waist", m_waist_depth, "vox", "depth at the waist, voxels"),
    ("quad.rump", m_rump_depth, "vox", "depth at the rump, voxels"),
    ("quad.belly", m_belly_share, "frac", "trunk mass above the outline mid"),
    ("quad.section", m_voxels, "vox", "solid voxels"),
    ("quad.hump", m_hump, "vox", "highest point above the mean back, vox"),
    ("quad.hump_at", m_hump_at, "frac", "highest point along the back"),
    ("quad.neck_thick", m_neck_thick, "vox", "neck cross-section, voxels"),
    ("quad.neck_taper", m_neck_vox, "vox", "neck voxels"),
    ("quad.mane", m_mane_vox, "vox", "mane voxels"),
    ("quad.dewlap", m_dewlap, "vox", "neck depth including the bell, vox"),
    ("quad.head_size", m_head_diam, "vox", "skull, columns"),
    ("quad.muzzle_depth", m_muzzle_depth, "vox", "muzzle depth, voxels"),
    ("quad.muzzle_width", m_muzzle_width, "vox", "muzzle width, voxels"),
    ("quad.muzzle_drop", m_muzzle_drop, "vox", "nose below the muzzle base, vox"),
    ("quad.jaw", m_muzzle_depth, "vox", "muzzle depth, voxels"),
    ("quad.eye", m_eye, "vox", "eye voxels"),
    ("quad.ear_len", m_ear_len, "vox", "ear reach from the skull, voxels"),
    ("quad.ear_width", m_ear_face, "vox", "ear outline seen side-on, columns"),
    ("quad.ear_deg", m_ear_span, "vox", "ears apart, voxels"),
    ("quad.ear_back", m_ear_back, "vox", "ears behind the skull, voxels"),
    ("quad.horn_len", m_horn_len, "vox", "headgear reach, voxels"),
    ("quad.horn_thick", m_horn_vox, "vox", "headgear voxels"),
    ("quad.horn_spread", m_horn_span, "vox", "headgear span, voxels"),
    ("quad.horn_curl", m_horn_back, "vox", "headgear carried back, voxels"),
    ("quad.horn_tines", m_horn_tines, "vox", "runs across the rack"),
    ("quad.tail_len", m_tail_len, "vox", "tail reach, voxels"),
    ("quad.tail_thick", m_tail_base, "vox", "tail cross-section at the base"),
    ("quad.tail_taper", m_tuft, "vox", "tail cross-section at the tip"),
    ("quad.tail_deg", m_tail_tip_h, "vox", "tail tip above the ground, vox"),
    ("quad.tail_arc", m_tail_rise, "vox", "tail bow off the straight, voxels"),
    ("quad.tail_tuft", m_tuft, "vox", "tail cross-section at the tip"),
    ("quad.leg_thick", m_leg_thick, "vox", "foreleg cross-section, voxels"),
    ("quad.fore_bend", m_foot_span, "vox", "fore feet to hind feet, columns"),
    ("quad.hock", m_hind_foot, "vox", "hind foot on the ground, columns"),
    ("quad.foot", m_leg_vox, "vox", "limb voxels"),
    ("quad.fore_reach", m_fore_gap, "vox", "fore foot off the ground, voxels"),
    ("quad.under", m_under_pct, "pct", "underparts colour, % of animal"),
    ("quad.cape", m_cape_pct, "pct", "marking colour, % of the trunk"),
    ("quad.stocking", m_stocking, "vox", "marking up the legs, voxels"),
    ("quad.tail_tip", m_tail_tip_pct, "pct", "marking, % of the tail"),
    ("quad.mark_count", m_mark_runs, "vox", "bands of marking"),
    ("quad.mark_width", m_mark_pct, "pct", "marking, % of animal"),
    ("quad.mark_strength", m_mark_pct, "pct", "marking, % of animal"),
]

# What a parameter needs SWITCHED ON before it can possibly do anything.
#
# Sweeping `horn_tines` on a hornless animal measures nothing and reports DEAD
# -- a false alarm, and a false alarm on a no-op detector is worse than no
# detector, because it teaches you to skim the output. The fish probe shipped
# exactly that on its first run.
#
# EVERY ENTRY HERE IS A COLOUR OR A SWITCH, NEVER A SIZE, with two named
# exceptions below. Setting up a sweep by making the animal bigger is how a
# probe flatters itself.
_MARK = {"materials.quad_mark": "plume_crimson"}
_HORN = {"quad.horn_shape": "sweep", "quad.horn_len": 0.45,
         "quad.horn_thick": 0.14, "materials.quad_horn": "plume_crimson"}
_EAR = {"quad.ear_shape": "blade", "quad.ear_len": 0.16}
SETUP_FOR = {
    "quad.mane": {"quad.mane": 0.5, **_MARK},
    "quad.dewlap": {"quad.dewlap": 0.6},
    # A position slider needs the thing whose position it sets. Swept on the
    # zebra, which authors no hump at all, `hump_at` moved the highest point of
    # a flat back by nothing and reported DEAD -- the textbook false alarm, and
    # the reason this table exists at all.
    "quad.hump_at": {"quad.hump": 0.6},
    "quad.horn_len": _HORN,
    "quad.horn_thick": _HORN,
    "quad.horn_spread": _HORN,
    "quad.horn_curl": {**_HORN, "quad.horn_shape": "curve"},
    "quad.horn_tines": {**_HORN, "quad.horn_shape": "branched",
                        "quad.horn_len": 0.55, "quad.horn_thick": 0.16},
    "quad.ear_len": _EAR,
    "quad.ear_width": _EAR,
    "quad.ear_deg": _EAR,
    "quad.ear_back": _EAR,
    "quad.eye": {"materials.quad_eye": "plume_lilac"},
    # The tail rows need a tail long enough to have a shape. A stub tail is two
    # voxels and every measurement of it is rounding -- which is a statement
    # about the measurement, not about the slider, so it is set up rather than
    # allowed to report DEAD.
    # WITH THE TUFT TURNED OFF, and that is a real finding about the generator
    # rather than a convenience. `quad.tail_tuft` draws a ball at the tip sized
    # from the tail's BASE thickness -- which is right, because a tuft is a
    # spray of hair whose size goes with the animal and not with the hair-thin
    # rod it grows out of -- and the zebra this sweep runs on authors one at
    # 0.60. So the tuft sat on the end of the tail at the same size whatever the
    # taper did, the tip measurement read 472 voxels at both ends of the slider,
    # and `tail_taper` reported DEAD on a species where it plainly works. The
    # two rows really do fight over the same voxels; the note is here so the
    # next person reads it as an interaction rather than as a bug.
    "quad.tail_taper": {"quad.tail_len": 0.6, "quad.tail_thick": 0.5,
                        "quad.tail_tuft": 0.0},
    "quad.tail_deg": {"quad.tail_len": 0.6},
    "quad.tail_arc": {"quad.tail_len": 0.7, "quad.tail_thick": 0.4},
    "quad.tail_tuft": {"quad.tail_len": 0.6, "quad.tail_thick": 0.18},
    "quad.tail_thick": {"quad.tail_len": 0.6},
    "quad.tail_tip": {"quad.tail_len": 0.6, "quad.tail_thick": 0.35, **_MARK},
    "quad.stocking": _MARK,
    "quad.cape": _MARK,
    "quad.under": {"materials.quad_belly": "plume_crimson"},
    "quad.muzzle_drop": {"quad.muzzle_frac": 0.20},
    # ON A LONGER ANIMAL, and that is a FINDING rather than a convenience --
    # the same one the bird probe records. A 0.7 m fox has a trunk twelve
    # voxels long; twenty-five bands across it is a period of half a voxel, so
    # 1 band and 25 bands both come out as "the whole animal is marked" and the
    # sweep reports the count slider DEAD. It is not dead, it is SATURATED. The
    # species that can carry many bands are the big ones.
    "quad.mark_count": {"quad.mark": "bars", "quad.length_m": 2.4, **_MARK},
    "quad.mark_width": {"quad.mark": "bars", "quad.length_m": 2.4, **_MARK},
    "quad.mark_strength": {"quad.mark": "spots", "quad.length_m": 2.4, **_MARK},
    # A fold is only visible on a limb long enough to fold. On a badger-legged
    # animal the whole range of `hock` moves the foot by one voxel.
    "quad.hock": {"quad.shoulder_h": 0.75},
    "quad.fore_bend": {"quad.shoulder_h": 0.75},
    "quad.fore_reach": {"quad.shoulder_h": 0.75},
}

# Choices are not numbers, so they are swept as a set rather than a range.
CHOICE_SWEEPS = [
    # THE WHOLE ANIMAL'S WIDTH, not the trunk's. Read off the trunk the three
    # stances measured 14.0, 14.0 and 14.0 and the row reported DEAD -- which
    # was true of what it measured and false of what it meant. A stance does not
    # change the body; it changes where the LIMBS go, and a sprawl puts them out
    # sideways.
    ("quad.stance", _q.STANCES, m_width, "vox", "width across, voxels", {}),
    ("quad.stance", _q.STANCES, m_fore_gap, "vox",
     "fore foot off the ground, voxels", {"quad.fore_reach": 0.45}),
    ("quad.ear_shape", _q.EARS, m_ear_face, "vox",
     "ear outline seen side-on, columns", {"quad.ear_len": 0.16}),
    ("quad.horn_shape", _q.HORNS, m_horn_back, "vox",
     "headgear carried back, voxels", _HORN),
    ("quad.horn_shape", _q.HORNS, m_horn_tines, "vox",
     "runs across the rack", {**_HORN, "quad.horn_len": 0.55,
                              "quad.horn_thick": 0.16}),
    ("quad.mark", _q.MARKS, m_mark_pct, "pct", "marking, % of animal",
     {"quad.length_m": 2.4, **_MARK}),
]


def _mean(spec, measure, seeds) -> float:
    vals = []
    for s in seeds:
        try:
            v = measure(pipeline.build(spec, s))
        except Exception as exc:                       # noqa: BLE001
            print(f"      build failed at seed {s}: {type(exc).__name__}: {exc}")
            continue
        if not np.isnan(v):
            vals.append(v)
    return float(np.mean(vals)) if vals else float("nan")


# Smallest change worth calling real, per unit of measurement. The one-voxel
# floor is the whole point of the exercise; applying it to a measurement whose
# units are a FRACTION would demand a change of 1.0, which no fraction can
# make, and the fish probe duly reported two working sliders as weak on exactly
# that basis.
FLOOR = {"vox": 1.0, "pct": 3.0, "frac": 0.03}


def _verdict(lo: float, hi: float, unit: str = "vox") -> tuple[str, float]:
    """DEAD unless the measurement moved by the unit's floor AND by 4%."""
    if np.isnan(lo) or np.isnan(hi):
        return "ERROR", 0.0
    absolute = abs(hi - lo)
    base = max(abs(lo), abs(hi), 1e-9)
    relative = absolute / base
    floor = FLOOR.get(unit, 1.0)
    if absolute < floor and relative < 0.04:
        return "DEAD", relative
    if absolute < floor or relative < 0.04:
        return "weak", relative
    return "moves", relative


def sweep(base: dict, seeds: list[int]) -> int:
    # SWEPT WITH INDIVIDUAL VARIATION TURNED OFF.
    #
    # Not to flatter the numbers -- to make them mean anything. Every land
    # animal spec authors `variation.height` at 0.09, so two seeds of one
    # species differ in length by several voxels of standard deviation, and a
    # muzzle is eight voxels long. Averaged over four seeds a working muzzle
    # slider sits inside that noise and this tool would call it DEAD, which is
    # the false alarm that makes a no-op detector worse than useless.
    base, _ = sm.patch(base, {"variation.amount": 0.0})
    print(f"\nPARAMETER SWEEPS on {sm.get(base, 'name')} "
          f"at {sm.get(base, 'resolution_cm')} cm, {len(seeds)} seeds "
          f"averaged, individual variation off\n")
    print(f"{'parameter':<22} {'measurement':<34} {'low':>9} {'high':>9} "
          f"{'change':>8}  verdict")
    dead = 0
    for path, measure, unit, label in SWEEPS:
        row = sm.BY_PATH[path]
        setup = SETUP_FOR.get(path, {})
        spec = sm.patch(base, setup)[0] if setup else base
        lo_v, hi_v = row.lo, row.hi
        if row.kind == "int":
            lo_v, hi_v = int(round(lo_v)), int(round(hi_v))
        a, _ = sm.patch(spec, {path: lo_v})
        b, _ = sm.patch(spec, {path: hi_v})
        lo = _mean(a, measure, seeds)
        hi = _mean(b, measure, seeds)
        state, rel = _verdict(lo, hi, unit)
        dead += state in ("DEAD", "ERROR")
        note = ("  [" + ", ".join(k.split(".")[-1] + "=" + str(v)
                                  for k, v in setup.items()) + "]") if setup else ""
        print(f"{path:<22} {label:<34} {lo:>9.2f} {hi:>9.2f} "
              f"{rel * 100:>7.0f}%  {state}{note}")

    print()
    for path, choices, measure, unit, label, setup in CHOICE_SWEEPS:
        spec = sm.patch(base, setup)[0] if setup else base
        vals = []
        for c in choices:
            s, _ = sm.patch(spec, {path: c})
            vals.append((c, _mean(s, measure, seeds)))
        good = [v for _, v in vals if not np.isnan(v)]
        if not good:
            print(f"{path:<22} {label:<34}       --  ERROR")
            dead += 1
            continue
        state, rel = _verdict(min(good), max(good), unit)
        dead += state in ("DEAD", "ERROR")
        detail = "  ".join(f"{c}={v:.1f}" for c, v in vals)
        print(f"{path:<22} {label:<34} {rel * 100:>7.0f}%  {state}")
        print(f"{'':<22} {detail}")
    return dead


def variation(base: dict, seeds: list[int]) -> None:
    """Do two individuals of one species actually differ?

    The failure this catches is specific and has happened here before: a
    quadruped reads `variation.*` in `quadruped._params` while `spec.realize`
    varies the TREE parameters, so a wiring mistake shows up as a herd of
    identical animals -- and a herd is the one place that is unmissable and the
    one place nobody looks until it is in the game.
    """
    print("\nVARIATION across seeds (a herd of clones is the failure)\n")
    for amount in (0.0, 1.0, 2.0):
        spec, _ = sm.patch(base, {"variation.amount": amount})
        lens = [m_length(pipeline.build(spec, s)) for s in seeds]
        deps = [m_body_depth(pipeline.build(spec, s)) for s in seeds]
        necks = [m_head_fwd(pipeline.build(spec, s)) for s in seeds]
        print(f"  variation.amount {amount:>4.1f}   "
              f"length {np.mean(lens):>6.1f} +/- {np.std(lens):>4.2f}   "
              f"depth {np.mean(deps):>5.1f} +/- {np.std(deps):>4.2f}   "
              f"head reach {np.mean(necks):>5.1f} +/- {np.std(necks):>4.2f}   "
              f"distinct lengths {len(set(lens))}/{len(lens)}")


# --- the four checks the sweep cannot make ----------------------------------


def stance(names: list[str], seeds: list[int]) -> int:
    """DO ALL THE FEET REACH THE FLOOR?

    This is the check `pipeline.health` cannot make. Its floating test reads
    `stats["ground_contact"]`, which is measured AFTER `pipeline.build` crops
    the grid to its occupied box -- so the bottom slab of a finished asset is
    occupied by definition and every quadruped in the library passes it whatever
    its legs did. An animal standing one voxel above its own feet would ship
    clean.

    A STANDING animal wants both gaps at 0. A BIPEDAL one wants the hind gap at
    0, the fore gap well clear, and the TAIL on the floor -- that is the tripod,
    and a third leg that does not reach the ground is not a third leg.
    """
    print("\nSTANCE: how far each foot is above the lowest voxel of the asset\n")
    print(f"{'species':<26} {'stance':<10} {'fore':>6} {'hind':>6} {'tail':>6} "
          f"{'foot span':>10}  verdict")
    bad = 0
    for n in names:
        s, _ = sm.load(SPECS / f"{n}.json")
        st = str(sm.get(s, "quad.stance"))
        fore = _mean(s, m_fore_gap, seeds)
        hind = _mean(s, m_hind_gap, seeds)
        tail = _mean(s, m_tail_tip_h, seeds)
        span = _mean(s, m_foot_span, seeds)
        why = []
        if hind > 1.5:
            why.append(f"hind feet {hind:.1f} vox off the ground")
        if st == "bipedal":
            if fore < 2.0:
                why.append("forelimbs are on the floor in a bipedal stance")
            if tail > 2.5:
                why.append(f"tail tip {tail:.1f} vox off the ground -- no tripod")
        else:
            if fore > 1.5 and float(sm.get(s, "quad.fore_reach")) >= 0.95:
                why.append(f"fore feet {fore:.1f} vox off the ground")
        bad += bool(why)
        print(f"{n:<26} {st:<10} {fore:>6.1f} {hind:>6.1f} {tail:>6.1f} "
              f"{span:>10.1f}  {'FAIL: ' + '; '.join(why) if why else 'ok'}")
    return bad


def rig(names: list[str], seeds: list[int]) -> int:
    """ARE FOUR LEGS FOUR PARTS?

    THE RAVEN'S-WINGS CHECK, one axis over. Measured on `common-raven` before
    `forge/parts.py` existed: tag `wing` covered 248 voxels left of the midline
    and 238 right, all under one id -- so a rigged raven could not flap, and
    nothing rendered wrong, because painting never asked the question. A
    quadruped has the same failure available twice: left against right, which
    `split_sides` handles, and FORE against HIND, which it cannot, because there
    is no line through a squirrel with the hips on one side and the shoulders on
    the other.

    So this asks four things of every species, and each of them has been false
    at some point in this file's short history:

      1. All four leg ids are present.
      2. Their voxel sets are disjoint -- one voxel, one part.
      3. Every part reports a joint. `forge.parts.joints` returns `origin: None`
         for a part touching its parent only at a corner, which is a rigging
         defect and would otherwise ship in silence.
      4. The fore joints are actually forward of the hind ones. Ids that are
         distinct but swapped would pass every other test here and animate an
         animal walking backwards.
    """
    print("\nRIG: four legs must be four parts, with four joints\n")
    names_by_id = partslib.names()
    print(f"{'species':<26} {'parts':>6} {'legs':>5} {'joints':>7} "
          f"{'fore-x':>7} {'hind-x':>7}  verdict")
    bad = 0
    for n in names:
        s, _ = sm.load(SPECS / f"{n}.json")
        a = pipeline.build(s, seeds[0])
        why = []
        if a.parts is None:
            print(f"{n:<26}   no part tags at all  FAIL")
            bad += 1
            continue
        present = {int(v) for v in np.unique(a.parts) if v}
        missing = [names_by_id[p] for p in LEGS if p not in present]
        if missing:
            why.append("missing " + ", ".join(missing))
        # Disjointness is guaranteed by the array being one id per voxel, so
        # what is actually worth checking is that none of the four is EMPTY or
        # a near-duplicate of another -- four ids over one leg's worth of
        # voxels would satisfy "all present" and animate as one limb.
        counts = {p: int((a.parts == p).sum()) for p in LEGS}
        if min(counts.values()) < 4:
            why.append("a leg under 4 voxels: " + ", ".join(
                f"{names_by_id[p]}={c}" for p, c in counts.items()))
        js = {j["part"]: j for j in partslib.joints(a.parts)}
        nojoint = [names_by_id[p] for p in sorted(js)
                   if js[p]["origin"] is None]
        if nojoint:
            why.append("no joint (corner contact only): " + ", ".join(nojoint))
        fore = [js[p]["origin"][0] for p in (LEG_FL, LEG_FR)
                if p in js and js[p]["origin"]]
        hind = [js[p]["origin"][0] for p in (LEG_HL, LEG_HR)
                if p in js and js[p]["origin"]]
        fx = float(np.mean(fore)) if fore else float("nan")
        hx = float(np.mean(hind)) if hind else float("nan")
        if not (fx > hx):
            why.append("fore joints are not forward of the hind ones")
        bad += bool(why)
        print(f"{n:<26} {len(present):>6} {sum(1 for p in LEGS if p in present):>5} "
              f"{len(js) - len(nojoint):>7} {fx:>7.1f} {hx:>7.1f}  "
              f"{'FAIL: ' + '; '.join(why) if why else 'ok'}")
    return bad


def caps(names: list[str], seeds: list[int]) -> None:
    """WAS THE JOINT BALL DRAWN, OR SKIPPED?

    A cap sphere at every joint, owned by the parent, skipped below three voxels
    of limb thickness (owner, 2026-08-14). Both outcomes are correct and neither
    is visible in a render -- the cap is inside the shoulder -- so the only way
    to know which happened is to count.

    Measured as the number of TRUNK voxels within a ball's radius of each hip
    and shoulder joint, against the same count with the caps suppressed. A
    difference of zero on an animal thick enough to have earned one is the
    silent no-op; a difference on a squirrel would mean the floor is not
    working and the cap is eating the leg.
    """
    print("\nJOINT CAPS: drawn above three voxels of limb, skipped below\n")
    print(f"{'species':<26} {'cm':>4} {'limb dia':>9} {'expected':>9} "
          f"{'trunk vox +':>12}  verdict")
    real = _q._limb_caps
    try:
        for n in names:
            s, _ = sm.load(SPECS / f"{n}.json")
            live, _ = sm.realize(s, pipeline.rng_for(s, seeds[0]))
            p = _q._params(live, pipeline.rng_for(s, seeds[0]),
                           float(sm.get(s, "resolution_cm")) / 100.0)
            dia = 2.0 * p["leg_r"]
            want = dia >= 3.0
            # AN A/B, NOT A COUNT ROUND THE JOINT, and the first version was the
            # count. The joints sit INSIDE the trunk by construction -- that is
            # the whole point of putting them at 0.80 of the local half-depth --
            # so a ball-shaped box around one is full of trunk whether or not a
            # cap was ever drawn. It duly reported a cap on every species with a
            # limb over a voxel thick, including the ones where the floor had
            # correctly suppressed it, and reported none on two species that had
            # one. That is a measurement that answers a different question and
            # then agrees with itself.
            #
            # Building the animal twice with the mechanism disabled is the only
            # thing that answers "did this do anything", and it is what the
            # whole file is for.
            # BUILT THROUGH THE GENERATOR AND NOT THE PIPELINE, because
            # `pipeline.build` CROPS to the occupied box and removing the caps
            # changes that box: the two grids came back 41 and 43 voxels tall
            # and could not be compared at all. Uncropped, both are exactly
            # `p["shape"]` and the layout's own coordinates address them, which
            # is what lets the count be restricted to the four joints.
            voxel_m = float(sm.get(s, "resolution_cm")) / 100.0
            o1: dict = {}
            _q.build(live, pipeline.rng_for(s, seeds[0]), voxel_m, out=o1)
            _q._limb_caps = lambda *_a, **_k: None
            o2: dict = {}
            _q.build(live, pipeline.rng_for(s, seeds[0]), voxel_m, out=o2)
            _q._limb_caps = real
            with_caps = o1["tags"] == partslib.P_BODY
            without = o2["tags"] == partslib.P_BODY
            # COUNTED ONLY ROUND THE FOUR LIMB JOINTS, and with only
            # `_limb_caps` suppressed. `_caps` draws three kinds of cap on three
            # separate gates -- a moose has limbs two voxels through and a neck
            # nine, so it correctly gets no shoulder cap and correctly does get
            # one at the withers. Suppressing the lot and diffing the whole body
            # reported 507 voxels for the moose and failed it for drawing a cap
            # it had not drawn; keeping the box but not the split still leaked
            # six voxels of NECK cap into a meerkat's shoulder box, because an
            # animal standing vertically carries its withers and its shoulder in
            # nearly the same place. Both halves of the fix are needed.
            box = np.zeros(with_caps.shape, bool)
            rad = p["leg_r"] * 1.45
            for at in (p["p_fore"], p["p_hind"]):
                for sgn in (-1.0, 1.0):
                    c = at + np.array([0.0, sgn * p["y_off"], 0.0])
                    x0, y0, z0 = (int(max(0, v - rad)) for v in c)
                    x1, y1, z1 = (int(v + rad) + 1 for v in c)
                    box[x0:x1, y0:y1, z0:z1] = True
            delta = int((with_caps & box).sum() - (without & box).sum())
            state = "ok" if (delta > 0) == want else (
                "FAIL: no cap where one was due" if want else
                "FAIL: cap drawn under the three-voxel floor")
            print(f"{n:<26} {sm.get(s, 'resolution_cm'):>4} {dia:>9.1f} "
                  f"{'drawn' if want else 'skipped':>9} {delta:>12} {state}")
    finally:
        _q._limb_caps = real


def lattice(names: list[str], seeds: list[int]) -> None:
    """Each species at its authored voxel size and one tier finer.

    The lattice recorded in a spec's notes is an ARGUMENT until something builds
    it. This prints the body length in voxels at both sizes and the cost of the
    finer one, which is what the fish and bird work did before committing to a
    tier. `docs/marine-megafauna-research.md` §5.3 has the equivalent table for
    fish, and the band it produced -- 28 to 294 voxels of length -- is the one
    to compare against.
    """
    tiers = [1.0, 2.0, 2.5, 5.0, 10.0]
    print("\nLATTICE: authored size against one tier finer\n")
    print(f"{'species':<26} {'cm':>5} {'length':>7} {'voxels':>9} "
          f"{'finer':>6} {'length':>7} {'voxels':>9}")
    for n in names:
        s, _ = sm.load(SPECS / f"{n}.json")
        cm = float(sm.get(s, "resolution_cm"))
        a = pipeline.build(s, seeds[0])
        finer = max((t for t in tiers if t < cm), default=None)
        if finer is None:
            print(f"{n:<26} {cm:>5g} {m_length(a):>7.0f} {a.stats['voxels']:>9,} "
                  f"{'--':>6} {'--':>7} {'--':>9}   already finest tier")
            continue
        b = pipeline.build(s, seeds[0], resolution_cm=finer)
        print(f"{n:<26} {cm:>5g} {m_length(a):>7.0f} {a.stats['voxels']:>9,} "
              f"{finer:>6g} {m_length(b):>7.0f} {b.stats['voxels']:>9,}")


def _luminance(rgb) -> float:
    def ch(v):
        v /= 255.0
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = (ch(float(c)) for c in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def _contrast(c1, c2) -> float:
    a, b = _luminance(c1), _luminance(c2)
    lo, hi = min(a, b), max(a, b)
    return (hi + 0.05) / (lo + 0.05)


def readability(names: list[str], seeds: list[int]) -> int:
    """CAN THE MARKING ACTUALLY BE SEEN?

    Value contrast, not hue, is what makes a marking read at this size -- the
    finding `docs/bird-colour-proposal.md` records and `tools/birdprobe.py
    --read` gates on. A marking measured against the wrong base colour passes
    every species in a library and would happily ship a white cape on a white
    animal, so this compares each marking against the colour it actually SITS
    ON: the upperparts for a cape or a body mark, the leg colour for stockings,
    the tail colour for a tail tip.

    The floor is 1.8, between the fish probe's 1.5 and the bird probe's 2.0. A
    mammal's markings are larger than a bird's wing bar -- a zebra stripe is a
    quarter of the animal's depth -- so they survive slightly less separation
    than a four-voxel bar does; and they are smaller than a fish's whole flank.
    """
    print("\nREADABILITY: value contrast of each marking against what it sits on\n")
    print(f"{'species':<26} {'what':<12} {'mark':<16} {'against':<16} "
          f"{'ratio':>6}  verdict")
    bad = 0
    for n in names:
        s, _ = sm.load(SPECS / f"{n}.json")
        mark = sm.get(s, "materials.quad_mark")
        mc = materials.color(materials.resolve(mark))
        rows = []
        if str(sm.get(s, "quad.mark")) != "none":
            rows.append(("marking", "back"))
        if float(sm.get(s, "quad.cape")) > 0.01:
            rows.append(("cape", "back"))
        if float(sm.get(s, "quad.stocking")) > 0.01:
            rows.append(("stockings", "leg"))
        if float(sm.get(s, "quad.tail_tip")) > 0.01:
            rows.append(("tail tip", "tail"))
        if float(sm.get(s, "quad.mane")) > 0.02:
            rows.append(("mane", "back"))
        if float(sm.get(s, "quad.under")) > 0.05:
            rows.append(("underparts", "back"))
        for what, against in rows:
            other = ("materials.quad_belly" if what == "underparts"
                     else "materials.quad_mark")
            om = sm.get(s, other)
            base = sm.get(s, f"materials.quad_{against}")
            ratio = _contrast(materials.color(materials.resolve(om)),
                              materials.color(materials.resolve(base)))
            ok = ratio >= 1.8
            bad += not ok
            print(f"{n:<26} {what:<12} {om:<16} {base:<16} {ratio:>6.2f}  "
                  f"{'ok' if ok else 'FAINT'}")
        if not rows:
            print(f"{n:<26} {'-- no markings authored':<52}")
    return bad


def sex(names: list[str], seeds: list[int]) -> int:
    """Male against female, on every species including the ones with no
    difference.

    THE NULLS ARE THE POINT. On a species authored with no dimorphism, "they
    look alike" and "the parameter never reached the build" are the same
    picture, and only running every species separates them. This also checks
    that a sex RESEEDS -- `quad.sex` is deliberately not in
    `spec.SEED_INVARIANT`, because there is no individual that is "the same red
    deer, but female" -- by comparing the two seed hashes rather than trusting
    the comment.
    """
    print("\nSEX: what actually changes between a male and a female\n")
    print(f"{'species':<26} {'reseeds':>8} {'length':>16} {'headgear':>18} "
          f"{'mane':>14}")
    bad = 0
    for n in names:
        s, _ = sm.load(SPECS / f"{n}.json")
        s, _ = sm.patch(s, {"variation.amount": 0.0})
        m, _ = sm.patch(s, {"quad.sex": "male"})
        f, _ = sm.patch(s, {"quad.sex": "female"})
        reseeds = sm.seed_hash(m) != sm.seed_hash(f)
        lm, lf = _mean(m, m_length, seeds), _mean(f, m_length, seeds)
        hm, hf = _mean(m, m_horn_vox, seeds), _mean(f, m_horn_vox, seeds)
        mm, mf = _mean(m, m_mane_vox, seeds), _mean(f, m_mane_vox, seeds)
        want = (float(sm.get(s, "quad.sex_length")) != 1.0
                or float(sm.get(s, "quad.sex_horn")) != 1.0
                or float(sm.get(s, "quad.sex_mane")) != 1.0)
        moved = (abs(lm - lf) > 1.0
                 or (not np.isnan(hm) and abs(np.nan_to_num(hm) - np.nan_to_num(hf)) > 1.0)
                 or abs(mm - mf) > 1.0)
        state = ""
        if not reseeds:
            state = "  FAIL: same seed hash for both sexes"
            bad += 1
        elif want and not moved:
            state = "  FAIL: a difference is authored and nothing moved"
            bad += 1
        elif not want and moved:
            state = "  note: no difference authored, but something moved"
        print(f"{n:<26} {'yes' if reseeds else 'NO':>8} "
              f"{lm:>7.1f} /{lf:>7.1f} {np.nan_to_num(hm):>8.0f} /"
              f"{np.nan_to_num(hf):>8.0f} {mm:>6.0f} /{mf:>6.0f}{state}")
    return bad


def _quad_specs() -> list[str]:
    out = []
    for p in sorted(SPECS.glob("*.json")):
        try:
            s, _ = sm.load(p)
        except (OSError, ValueError):
            continue
        if sm.get(s, "kind") == "quadruped":
            out.append(p.stem)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--spec", default="plains-zebra",
                    help="which species the parameter sweep runs on")
    ap.add_argument("--seeds", type=int, default=4)
    ap.add_argument("--stance", action="store_true")
    ap.add_argument("--parts", action="store_true")
    ap.add_argument("--caps", action="store_true")
    ap.add_argument("--lattice", action="store_true")
    ap.add_argument("--read", action="store_true")
    ap.add_argument("--sex", action="store_true")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    seeds = list(range(1, args.seeds + 1))
    names = _quad_specs()
    if not names:
        print("no quadruped specs found", file=sys.stderr)
        return 2

    picked = any((args.stance, args.parts, args.caps, args.lattice,
                  args.read, args.sex))
    failures = 0
    if args.all or not picked:
        base, _ = sm.load(SPECS / f"{args.spec}.json")
        failures += sweep(base, seeds)
        variation(base, seeds)
    if args.all or args.stance:
        failures += stance(names, seeds)
    if args.all or args.parts:
        failures += rig(names, seeds)
    if args.all or args.caps:
        caps(names, seeds)
    if args.all or args.lattice:
        lattice(names, seeds)
    if args.all or args.read:
        failures += readability(names, seeds)
    if args.all or args.sex:
        failures += sex(names, seeds)

    print(f"\nquadprobe: {failures} thing{'s' if failures != 1 else ''} to look at")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
