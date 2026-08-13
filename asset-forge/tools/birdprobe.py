"""Does each bird slider actually do anything? Measure it and say so.

THE SIGNATURE FAILURE OF THIS PROJECT IS THE SILENT NO-OP. A weathering pass
removed 20 voxels out of 90,000 for months while reporting success. A `--fit`
flag produced byte-identical images. A slider's value was consumed twenty-five
lines before the code that modified it. Every one of those ran, cost time, and
changed nothing, and every one was found by a person looking at a picture.

So every mechanism in `forge/bird.py` gets a number here that moves when its
slider moves, and this tool prints the number and says DEAD when it does not.

    python tools/birdprobe.py                 # sweep every bird parameter
    python tools/birdprobe.py --seeds 6       # more seeds, tighter answer
    python tools/birdprobe.py --read          # the readability tests
    python tools/birdprobe.py --lattice       # 1 cm against 2 cm and 5 cm
    python tools/birdprobe.py --pose          # perched against flying
    python tools/birdprobe.py --pose-ab       # ... and the render of it
    python tools/birdprobe.py --sex           # male against female
    python tools/birdprobe.py --sex-ab        # ... and the render of it

WHY IT AVERAGES OVER SEEDS. Changing any parameter changes the seed hash, and
`pipeline.rng_for` mixes the hash into the seed -- so a one-seed A/B is not the
same bird twice, it is two different birds. Four seeds is enough to see a real
effect through that; one is enough to invent one.

THE ONE PARAMETER THAT IS NO LONGER LIKE THAT IS `bird.pose`. It is the single
entry in `spec.SEED_INVARIANT`, so it is normalised out of the hash
`pipeline.rng_for` seeds from and the two poses of a species are one individual.
That is what `--pose` tests and it is the only A/B in this file that is the same
bird by construction rather than by averaging.

`bird.sex` IS DELIBERATELY NOT LIKE THAT, and `--sex` checks it. A pose is a
posture and a sex is not: there is no individual that is "the same mallard, but
female". So a male and a female of one seed are two different birds, every
species is checked for it including the twelve with no dimorphism at all -- on
those it is the only thing separating "they look alike" from "the parameter
never reached the build" -- and the sex table pins `variation.amount` to zero so
that what is left is the mechanism and not the draw.

WHAT "MOVES" MEANS HERE. A parameter passes if its measurement changes by more
than one voxel AND by more than 4% across its authored range. Both, because
either alone lies: a percentage is meaningless on a quantity that is 2 voxels,
and one voxel of change on a 60-voxel measurement is rounding.

WHAT IS DIFFERENT FROM THE FISH PROBE, and it matters. A fish is one solid, so
almost every measurement can be taken off the whole silhouette. A bird is six
parts, and a measurement taken off the whole animal is usually dominated by the
wrong one -- the tail is longer than the body, the wings are wider than
everything, and the bill is four voxels out of six hundred. So most of the
measurements below read the MATERIAL HISTOGRAM or a REGION of the silhouette
rather than the whole outline, and each one says which part it is looking at
and what it was measuring before that turned out to be the wrong part.

READ THE COMMENTS ON THE MEASUREMENTS, NOT JUST THE VERDICTS. Six of them are
on their second or third version, and in every case the first version reported
a working parameter as DEAD -- `bird.tail_frac`, `bird.tail_width`,
`bird.mark_count`, `bird.neck_frac`, `bird.wing_fold` and `bird.wing_shape` all
measured as doing nothing while plainly working in a render. Two more lied the
OTHER way, which is worse: `m_head_mark` counted 74% of a herring gull as head
marking on a species that carries none, and the ink measurement then compared
two builds that were different individuals because turning a marking off
changes the spec hash. A probe that reassures is worse than no probe.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

import _path  # noqa: F401  (sys.path bootstrap)
from forge import bird as _birdlib, cli, materials, pipeline, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"


# --- measurements -----------------------------------------------------------
#
# Each of these is a number a HUMAN could check off a render, which is the
# point: a probe that measures an internal variable proves the variable exists,
# not that it reached the voxels.


def _bird_mat(spec: dict, slot: str) -> int:
    """The material one bird slot is ACTUALLY painted in, after the sex swap.

    EVERY MEASUREMENT IN THIS FILE THAT READS A COLOUR HAS TO GO THROUGH HERE,
    and the reason is a measurement that read 0 within an hour of the swap
    existing. `m_bill_run` finds the bill by looking for the bill COLOUR, and
    it took that colour straight off `materials.bird_bill` -- which on a hen
    mallard is the drake's yellow, a colour she does not wear anywhere. It duly
    reported her bill as nought columns long on a bird that has one. Nothing
    was wrong with the bird.

    That is the same class of failure as `m_head_mark` counting 74% of a
    herring gull as head marking: a probe reading the spec where it should be
    reading the animal. `bird._alt_mat` is the drawing code's own resolution,
    so this cannot drift from what was painted.

    THE EYE IS NOT AN `alt` SLOT and falls through to a plain resolve. See the
    note on the `materials.bird_alt_*` block in spec.py for why it does not
    have one.
    """
    if slot in _birdlib.ALT_SLOTS:
        return _birdlib._alt_mat(spec, slot, _birdlib._alt(spec))
    return materials.resolve(sm.get(spec, f"materials.bird_{slot}"))


def _bird_mark(spec: dict, which: str) -> str:
    """The marking one region ACTUALLY carries, after the sex swap. Same reason
    as `_bird_mat`: a female kestrel is barred where the spec says spotted."""
    return _birdlib._alt_mark(spec, which, _birdlib._alt(spec))


def _occ(a):
    return a.grid.data != 0


def _side(a):
    """The silhouette seen broadside: (x, z), which is the preview camera."""
    return _occ(a).any(axis=1)


def _plan(a):
    """The silhouette seen from above: (x, y)."""
    return _occ(a).any(axis=2)


def m_length(a) -> float:
    """Bill tip to tail tip along the grid's long axis, in voxels."""
    return float(_occ(a).shape[0])


def m_height(a) -> float:
    return float(_occ(a).shape[2])


def m_span(a) -> float:
    """How far the animal reaches across, in voxels. Wings, when it has them."""
    return float(_occ(a).shape[1])


def m_voxels(a) -> float:
    return float(a.stats["voxels"])


def m_body_depth(a) -> float:
    """Depth of the body, in voxels, ignoring head, tail and legs.

    The MEDIAN column depth over the middle third of the animal, not the
    maximum. A bird's tallest column is usually its head or its own leg, so a
    maximum answered "how deep is the body" with the head's diameter and
    reported `bird.body_depth` as weaker than it is.
    """
    sil = _side(a)
    n = sil.shape[0]
    cols = sil[int(0.35 * n):int(0.65 * n)].sum(axis=1)
    cols = cols[cols > 0]
    return float(np.median(cols)) if cols.size else 0.0


def m_body_width(a) -> float:
    plan = _plan(a)
    n = plan.shape[0]
    w = plan[int(0.35 * n):int(0.65 * n)].sum(axis=1)
    w = w[w > 0]
    return float(np.median(w)) if w.size else 0.0


def _tail_region(a):
    """Which columns from the rear of the grid are tail, and nothing else.

    THE TAIL IS THE THIN-ACROSS PART, and which way "across" points depends on
    the pose: a folded tail is a vertical blade one or two voxels thick in y,
    and a spread one is a horizontal fan one or two voxels thick in z. The body
    is thick in both. So the region is the run of aft columns whose extent
    across the fan's own thin axis is at most three voxels.

    TWO EARLIER VERSIONS OF THIS WERE WRONG AND BOTH LOOKED RIGHT. Measuring
    "columns thinner than the median column" failed on a robin, whose tail
    blade is three voxels tall against a five-voxel median, and reported
    `bird.tail_frac` as DEAD over its whole range. Measuring against the DEEPEST
    column instead then failed the other way on a wide fan: a square tail 0.9 of
    its own length across is TALLER than the body it hangs off, so the region
    collapsed to five columns on a forty-voxel tail. Thinness across the blade
    is the only property a tail has that a body does not, in either pose.
    """
    occ = _occ(a)
    flying = sm.get(a.spec, "bird.pose") == "flying"
    # Thin in Z when the fan is spread, thin in Y when it is folded. Written
    # the other way round first, which measured the tail's DEPTH -- the very
    # thing the fan varies -- and collapsed the region to two columns.
    across = occ.any(axis=1) if flying else occ.any(axis=2)   # (x, thin axis)
    thin = across.sum(axis=1)
    limit = max(3, int(sm.get(a.spec, "bird.tail_thick", 1)) + 1)
    run = 0
    for x in range(occ.shape[0]):
        if thin[x] == 0 or thin[x] > limit:
            break
        run += 1
    return _side(a), run


def m_belly_share(a) -> float:
    """Where the body's mass sits inside its own outline, 0 at the bottom and
    1 at the top.

    `bird.belly` moves depth from above the body axis to below it. It is a
    REDISTRIBUTION, so the voxel count barely changes -- 461 against 450 across
    the whole range, which reads as a slider doing nothing and is a slider
    doing exactly what it says. Where the mass sits is what it controls.
    """
    sil = _side(a)
    n = sil.shape[0]
    mid = sil[int(0.35 * n):int(0.65 * n)]
    if not mid.any():
        return 0.0
    zs = np.nonzero(mid)[1].astype(float)
    lo, hi = zs.min(), zs.max()
    if hi - lo < 1.0:
        return 0.0
    return float((zs.mean() - lo) / (hi - lo))


def m_tail_run(a) -> float:
    """How many columns behind the body are tail. See `_tail_region`."""
    return float(_tail_region(a)[1])


def m_tail_span(a) -> float:
    """How far the tail fan spreads across itself, in voxels.

    Taken over the aft 15% of the grid first, which is the tail's TIP -- the
    narrowest part of every shape except a square one -- so `bird.tail_width`
    measured as DEAD.
    """
    occ = _occ(a)
    _, run = _tail_region(a)
    if run < 1:
        return 0.0
    flying = sm.get(a.spec, "bird.pose") == "flying"
    fan = occ[:run].any(axis=2) if flying else occ[:run].any(axis=1)
    return float(fan.sum(axis=1).max()) if fan.any() else 0.0


def m_tail_notch(a) -> float:
    """How much further aft the OUTER tail feathers reach than the central
    pair, in voxels. Signed: positive is forked, negative is graduated.

    AFT IS THE GRID'S -X AND THE FAN'S LATERAL AXIS IS FOUND, which is the pair
    that finally worked after three that did not.

    Aft is known: the bird is drawn bill-first at +x, so a feather that reaches
    further aft has a smaller x. There is no need to discover that and two
    earlier versions discovered it wrongly.

    The lateral axis is NOT known, and it is not a grid axis. A folded tail
    stands along the body's own up direction, which on a robin authored at 42
    degrees nose-up is a diagonal in x and z together; a spread one lies along
    y. So it is taken from the two principal axes of the tail's voxels, picking
    whichever is more nearly perpendicular to x -- because the other one is the
    tail's length, and on a broad fan the length is not even the longest axis,
    which is why using the first principal axis for it gave square = -14.2 and
    forked = -14.9, two shapes that are opposites reading as the same number.
    """
    occ = _occ(a)
    _, run = _tail_region(a)
    if run < 3:
        return 0.0
    pts = np.array(np.nonzero(occ[:run]), dtype=float).T
    if len(pts) < 12:
        return 0.0
    centred = pts - pts.mean(axis=0)
    _, _, vt = np.linalg.svd(centred, full_matrices=False)
    lateral = min(vt[:2], key=lambda v: abs(v[0]))
    c = centred @ lateral
    lo, hi = c.min(), c.max()
    if hi - lo < 3.0:
        return 0.0
    edge = (hi - lo) / 3.0
    x = pts[:, 0]
    mid = x[np.abs(c) <= edge * 0.5]
    out = x[np.abs(c) >= edge]
    if mid.size == 0 or out.size == 0:
        return 0.0
    # A smaller x is further aft, so the centre stopping short is positive.
    #
    # HOW FAR TO TRUST THE SIGN. It separates the seven shapes -- they measure
    # -6, 0, +5, 0, +4, +11 and +6 -- which is what this file is for: it proves
    # `bird.tail_shape` reaches the voxels. It does NOT reproduce the
    # field-guide ordering, in which graduated should be the negative of
    # forked, and it does not because the tail region still contains the folded
    # wing's projecting primaries, which lie along the tail and end wherever
    # `bird.wing_fold` puts them. Separating those needs a per-part mask the
    # probe does not have, since the tail and the wing are painted in one
    # material by design. Read this as a magnitude, not as a direction, and use
    # a render for the direction.
    return float(np.min(mid) - np.min(out))


def m_neck_run(a) -> float:
    """How far the head sits from the shoulders, in voxels.

    Measured as the straight-line distance from the centre of the head-coloured
    voxels to the centre of the whole animal, which is what a neck DOES: it
    pushes the head away from the body. That is only meaningful where the head
    colour differs from the body's, so `SETUP_FOR` forces one that does.

    IT WAS A WAIST MEASUREMENT FIRST -- "how many columns forward of the body
    are thinner than two thirds of it" -- and that measured DEAD across the
    whole range. Two reasons, and both are instructive. A heron's neck is thin
    AND long, so a waist count sees it; a duck's is thick and long and there is
    no waist to count at all. And on a bird at any posture the neck leaves the
    shoulders at an angle, so it does not occupy its own columns -- it shares
    them with the breast underneath it. Distance has neither problem.
    """
    mat = _bird_mat(a.spec, "head")
    m = a.grid.data == mat
    if not m.any():
        return 0.0
    # AGAINST THE TAIL TIP, which is column zero of a cropped grid, and not
    # against the animal's own centroid. The five length shares are NORMALISED,
    # so lengthening the neck shortens everything else and drags the centroid
    # forward with the head -- the two motions cancel and a slider that moves
    # the head half the length of the bird measured as 6%.
    head = np.array(np.nonzero(m), dtype=float).mean(axis=1)
    return float(head[0])


def m_head_diam(a) -> float:
    """Head diameter, in voxels: the widest column of head-coloured voxels.

    Reads the MATERIAL, not the geometry, because a head and a body run
    together on a short-necked bird and no silhouette measurement can tell
    them apart. This is only meaningful where the head colour differs from the
    body's, which is why `SETUP_FOR` forces one that does.
    """
    mat = _bird_mat(a.spec, "head")
    m = (a.grid.data == mat).any(axis=1)
    if not m.any():
        return 0.0
    return float(m.sum(axis=1).max())


def m_bill_run(a) -> float:
    """How many columns forward of the head carry bill and nothing else."""
    mat = _bird_mat(a.spec, "bill")
    cols = (a.grid.data == mat).any(axis=(1, 2))
    if not cols.any():
        return 0.0
    xs = np.flatnonzero(cols)
    # Only the FORWARD run: the same material draws the legs, and on a heron
    # the legs are three times the bill. Taking the extent of every bill-
    # coloured voxel measured the leg length and reported `bird.bill_frac` as
    # a working slider that moved by 4%.
    run = 0
    x = int(xs[-1])
    while x >= 0 and cols[x]:
        run += 1
        x -= 1
    return float(run)


def m_bill_drop(a) -> float:
    """How far the bill's tip sits below its base, in voxels.

    This is the one that caught `bill_curve` being applied to the bill's
    DIRECTION rather than to its centreline. Both produce a tip below the base,
    so this measurement alone cannot tell them apart -- `m_bill_bend` is the
    partner that can, and the two of them disagreeing is the signature.
    """
    mat = _bird_mat(a.spec, "bill")
    m = a.grid.data == mat
    xs = np.flatnonzero(m.any(axis=(1, 2)))
    if xs.size < 2:
        return 0.0
    front = int(xs[-1])
    back = int(xs[-1]) - max(1, (int(xs[-1]) - int(xs[0])) // 2)
    zf = np.flatnonzero(m[front].any(axis=0))
    zb = np.flatnonzero(m[back].any(axis=0))
    if zf.size == 0 or zb.size == 0:
        return 0.0
    return float(zb.mean() - zf.mean())


def m_bill_bend(a) -> float:
    """How far the bill's MIDDLE departs from the straight line joining its
    ends, in voxels. Zero for a straight bill however it is aimed.

    A straight bill pointing downhill and a decurved bill both drop their tip
    by the same amount and both look like a bill in a render. This is the
    number that separates them, and it is the reason `bill_curve` bends a
    centreline rather than rotating a direction.
    """
    mat = _bird_mat(a.spec, "bill")
    m = a.grid.data == mat
    xs = np.flatnonzero(m.any(axis=(1, 2)))
    if xs.size < 4:
        return 0.0
    lo, hi = int(xs[0]), int(xs[-1])

    def zbar(x: int) -> float:
        z = np.flatnonzero(m[x].any(axis=0))
        return float(z.mean()) if z.size else float("nan")

    z0, z1 = zbar(hi), zbar(lo)
    if np.isnan(z0) or np.isnan(z1):
        return 0.0
    # SIGNED. Taken as an absolute value first, which made a recurved bill and
    # a decurved one measure the same and reported `bird.bill_curve` as moving
    # by 9% across a range whose two ends are opposite shapes. Positive is
    # decurved -- the middle of the bill rides above the line joining its ends.
    worst = 0.0
    for x in range(lo + 1, hi):
        z = zbar(x)
        if np.isnan(z):
            continue
        f = (x - lo) / max(hi - lo, 1)
        d = z - (z1 + (z0 - z1) * f)
        if abs(d) > abs(worst):
            worst = d
    return float(worst)


def m_bill_depth(a) -> float:
    """How deep the bill is at its base, in voxels.

    `bird.bill_depth` is four voxels of change on a six-hundred-voxel bird, so
    measuring it by total voxel count reported it as weak at 3% while it is the
    whole difference between a finch and a warbler. This measures the bill.
    """
    mat = _bird_mat(a.spec, "bill")
    m = a.grid.data == mat
    xs = np.flatnonzero(m.any(axis=(1, 2)))
    if xs.size < 2:
        return 0.0
    run = int(m_bill_run(a))
    x = max(0, int(xs[-1]) - max(0, run - 1))
    z = np.flatnonzero(m[x].any(axis=0))
    return float(z.size)


def m_bill_width(a) -> float:
    mat = _bird_mat(a.spec, "bill")
    m = (a.grid.data == mat).any(axis=2)
    return float(m.sum(axis=1).max()) if m.any() else 0.0


def m_posture(a) -> float:
    """Slope of the body's own long axis, as rise over run.

    Reads the tilt off the silhouette by fitting a line through the body
    columns' centres of mass. Measuring the grid's height instead confounds
    posture with everything that hangs off the bird -- a heron's legs are a
    third of its height and do not tilt.
    """
    sil = _side(a)
    n = sil.shape[0]
    lo, hi = int(0.30 * n), int(0.75 * n)
    xs, zs = [], []
    for x in range(lo, hi):
        z = np.flatnonzero(sil[x])
        if z.size:
            xs.append(x)
            zs.append(z.mean())
    if len(xs) < 4:
        return 0.0
    # x runs tail-to-bill in the grid, so a nose-up bird has a positive slope.
    return float(np.polyfit(np.array(xs, float), np.array(zs, float), 1)[0])


def m_leg_drop(a) -> float:
    """How far the legs hang below the body, in voxels."""
    mat = _bird_mat(a.spec, "bill")
    m = a.grid.data == mat
    if not m.any():
        return 0.0
    sil = _side(a)
    cols = sil.sum(axis=1)
    bottoms = np.array([np.flatnonzero(sil[:, z]).size for z in range(sil.shape[1])])
    zs = np.flatnonzero(m.any(axis=(0, 1)))
    if zs.size == 0 or not cols.any():
        return 0.0
    # Distance from the lowest bill/leg voxel up to where the silhouette gets
    # thick again, which is the belly line.
    thick = np.flatnonzero(bottoms > 0.35 * bottoms.max())
    if thick.size == 0:
        return 0.0
    return float(max(0, int(thick[0]) - int(zs[0])))


def m_crest(a) -> float:
    """How far the tallest thing on the head stands above the head's own top."""
    sil = _side(a)
    n = sil.shape[0]
    fwd = sil[int(0.60 * n):]
    if not fwd.size or not fwd.any():
        return 0.0
    tops = np.array([np.flatnonzero(c)[-1] if c.any() else 0 for c in fwd])
    tops = tops[tops > 0]
    if tops.size == 0:
        return 0.0
    return float(tops.max() - np.median(tops))


def m_wing_reach(a) -> float:
    """Half the span, minus the body's own width, in voxels.

    The only thing on a bird that sticks out sideways is a wing, so "how much
    wider than the body is the widest slice" measures a spread wing and
    nothing else.
    """
    plan = _plan(a)
    w = plan.sum(axis=1)
    w = w[w > 0]
    if w.size == 0:
        return 0.0
    return float((w.max() - np.median(w)) * 0.5)


def _chord_at(a, frac: float) -> float:
    occ = _occ(a)
    ny = occ.shape[1]
    yc = ny // 2
    ys = np.flatnonzero(occ.any(axis=(0, 2)))
    if ys.size < 6:
        return 0.0
    y = int(yc + (int(ys[-1]) - yc) * frac)
    if not 0 <= y < ny:
        return 0.0
    return float(occ[:, y, :].any(axis=1).sum())


def m_wing_chord(a) -> float:
    """The wing's chord out at half span, in voxels.

    Measured at the HALF-SPAN station rather than at the root, because at the
    root the wing and the body are the same columns and no measurement can
    separate them. This is the only thing that can see `bird.wing_aspect`.
    """
    return _chord_at(a, 0.55)


def m_wing_taper(a) -> float:
    """Chord at half span minus chord at 85% of it, in voxels.

    THE FOUR PLANFORMS ALL HAVE THE SAME MEAN CHORD BY CONSTRUCTION, because
    the root chord is normalised out of the aspect ratio -- so a measurement at
    any single station cannot tell them apart, and the choice reported as
    moving by 10%, which is inside the noise. What differs between them is how
    the chord is DISTRIBUTED, and a distribution needs two stations to see.
    """
    return _chord_at(a, 0.55) - _chord_at(a, 0.85)


def m_wing_sweep(a) -> float:
    """How far back the wingtip is carried against the wing root, in voxels."""
    occ = _occ(a)
    ny = occ.shape[1]
    yc = ny // 2
    ys = np.flatnonzero(occ.any(axis=(0, 2)))
    if ys.size < 6:
        return 0.0

    def centre(y: int) -> float:
        col = np.flatnonzero(occ[:, y, :].any(axis=1))
        return float(col.mean()) if col.size else float("nan")

    inner = centre(int(yc + (int(ys[-1]) - yc) * 0.25))
    outer = centre(int(yc + (int(ys[-1]) - yc) * 0.85))
    if np.isnan(inner) or np.isnan(outer):
        return 0.0
    return float(inner - outer)


def m_wing_rise(a) -> float:
    """How far the wingtip is carried above the wing root, in voxels."""
    occ = _occ(a)
    ny = occ.shape[1]
    yc = ny // 2
    ys = np.flatnonzero(occ.any(axis=(0, 2)))
    if ys.size < 6:
        return 0.0

    def height(y: int) -> float:
        col = np.flatnonzero(occ[:, y, :].any(axis=0))
        return float(col.mean()) if col.size else float("nan")

    inner = height(int(yc + (int(ys[-1]) - yc) * 0.25))
    outer = height(int(yc + (int(ys[-1]) - yc) * 0.85))
    if np.isnan(inner) or np.isnan(outer):
        return 0.0
    return float(outer - inner)


def m_wing_gaps(a) -> float:
    """How many separate runs of solid the wingtip is broken into.

    This is the ONLY measurement that can see `bird.wing_slots`. A slotted
    wingtip is separated primary feathers with daylight between them, and
    daylight is the whole point -- counting the tip's voxels instead measured
    how much feather there is, which barely moves when it is cut into six.
    """
    occ = _occ(a)
    nx, ny, nz = occ.shape
    yc = ny // 2
    ys = np.flatnonzero(occ.any(axis=(0, 2)))
    if ys.size < 8:
        return 0.0
    y = int(yc + (int(ys[-1]) - yc) * 0.92)
    if not 0 <= y < ny:
        return 0.0
    col = occ[:, y, :].any(axis=1).astype(np.int8)
    return float(np.count_nonzero(np.diff(np.concatenate(([0], col))) > 0))


def m_wing_fold(a) -> float:
    """How many columns short of the tail tip the folded wingtips stop.

    A perched bird's tail is a VERTICAL blade one or two voxels thick, so
    everything aft of the body sits on the mid-plane -- except the projecting
    primary tips, which are drawn out at plus and minus four tenths of the
    body's width. So the aft-most column containing anything more than two
    voxels off the mid-plane IS the wingtip, and nothing else is.

    A SMALLER NUMBER IS A LONGER WING. The grid is cropped, so column zero is
    the tail tip.

    TWO EARLIER VERSIONS MEASURED NOTHING. Counting wing-COLOURED voxels aft of
    the body cannot work, because `bird._paint` draws the tail in the wing's
    colour by design -- a bird's tail and its flight feathers are the same
    plumage -- and the tail is four times the length of the primaries, so it
    drowned them. Measuring the width of the tail REGION cannot work either,
    because `_tail_region` is defined as the run of columns that are THIN
    across, and the primaries are precisely the thing that makes a column wide:
    the region stops exactly where the wingtip starts. Both reported 3.00 at
    both ends of the slider.
    """
    occ = _occ(a)
    nx, ny, nz = occ.shape
    ys = np.abs(np.arange(ny, dtype=np.float64) - (ny - 1) * 0.5)
    off = occ & (ys >= 2.0)[None, :, None]
    xs = np.flatnonzero(off.any(axis=(1, 2)))
    if xs.size == 0:
        return float(nx)
    return float(xs[0])


def _share(a, slot: str) -> float:
    """Share of the bird wearing one slot's colour. Slot NAME, not parameter
    path, so that it resolves through the sex swap -- see `_bird_mat`."""
    mat = _bird_mat(a.spec, slot)
    hist = a.stats["by_material"]
    return 100.0 * hist.get(mat, 0) / max(sum(hist.values()), 1)


def m_upper(a) -> float:
    return _share(a, "back")


def m_under(a) -> float:
    return _share(a, "belly")


def m_mark(a) -> float:
    """Share of the bird wearing the wing/body marking material.

    Zero here with a marking selected is the exact shape of the silent no-op
    this file exists to catch. Reported as zero on purpose when the marking
    material is the same as the thing it sits on, because a mark painted in
    the base colour covers voxels and shows nothing.
    """
    if (_bird_mark(a.spec, "wing_mark") == "none"
            and _bird_mark(a.spec, "body_mark") == "none"):
        return 0.0
    mat = _bird_mat(a.spec, "mark")
    # AGAINST EVERY OTHER SLOT, not just the wing and the back. A robin's
    # marking colour and its underparts colour are both `plume_white`, so with
    # only two of the six checked this counted the whole belly as marking and
    # reported 45% of the bird as barred on a species that carries no bar at
    # all -- and then reported the same 45% at one bar and at ten, which reads
    # as a dead slider and is a dead measurement.
    others = {_bird_mat(a.spec, k)
              for k in ("wing", "back", "belly", "head")}
    if mat in others:
        return 0.0
    return _share(a, "mark")


def m_head_mark(a) -> float:
    """Share of the bird wearing the head-marking material.

    THE "none" GUARD IS NOT A FORMALITY. Without it this counted every voxel of
    the head-marking colour whether or not a head marking was selected -- and on
    a herring gull, whose head-mark slot happens to hold the same grey its back
    and wings are painted in, it reported 66% of the animal as head marking on a
    species that carries none at all. The readability table duly showed the gull
    at 74% ink and nothing looked wrong. A probe that lies in the reassuring
    direction is worse than no probe.
    """
    if _bird_mark(a.spec, "head_mark") == "none":
        return 0.0
    mat = _bird_mat(a.spec, "head_mark")
    head = _bird_mat(a.spec, "head")
    if mat == head:
        return 0.0
    return _share(a, "head_mark")


def m_mark_runs(a) -> float:
    """How many separate lumps of marking there are.

    COUNTED AS CONNECTED COMPONENTS, not as runs along the grid's x axis. Bars
    are drawn against the BODY'S OWN axis, which is tilted, so on a bird at any
    posture at all they run diagonally across x and z together and every single
    x column contains some marking -- one run, whatever the count. The sweep
    duly reported `bird.mark_count` as DEAD at 1 bar and at 24 alike, which is
    true of the measurement and false of the code.

    `bird.mark_count` sets the NUMBER of bars while `bird.mark_width` sets how
    much of each period is bar, so at a fixed width more bars cover about the
    same total area -- which is correct, and which is why coverage cannot see
    the count either. This is the third measurement tried for one parameter.
    """
    from scipy import ndimage

    mat = _bird_mat(a.spec, "mark")
    m = a.grid.data == mat
    if not m.any():
        return 0.0
    return float(ndimage.label(m, structure=np.ones((3, 3, 3), bool))[1])


def m_eye(a) -> float:
    """Eye voxels -- but ONLY where the eye colour is unique to the eye.

    Nine of the twenty species paint their eye in a colour something else on
    the bird also wears: a heron's eye and its bill are both yellow, a gull's
    eye and its head are both white. Counting the material then counts the
    head, and the lattice table duly reported a herring gull with 1,432 eye
    voxels. NaN is reported instead, which prints as a dash, because "I cannot
    measure this here" and "there is no eye" are different facts and the second
    one is what decides the lattice.
    """
    eye = _bird_mat(a.spec, "eye")
    others = {_bird_mat(a.spec, k)
              for k in ("back", "belly", "head", "wing", "mark", "head_mark",
                        "bill")}
    if eye in others:
        return float("nan")
    return float(a.stats["by_material"].get(eye, 0))


# parameter -> (measurement, unit, label). Swept low against high.
SWEEPS = [
    ("bird.length_m", m_length, "vox", "length, voxels"),
    ("bird.bill_frac", m_bill_run, "vox", "bill, columns"),
    ("bird.head_frac", m_head_diam, "vox", "head diameter, voxels"),
    ("bird.neck_frac", m_neck_run, "vox", "head forward of the tail, vox"),
    ("bird.body_frac", m_body_depth, "vox", "body depth, voxels"),
    ("bird.tail_frac", m_tail_run, "vox", "tail, columns"),
    ("bird.posture_deg", m_posture, "frac", "body slope, rise/run"),
    ("bird.body_depth", m_body_depth, "vox", "body depth, voxels"),
    ("bird.body_width", m_body_width, "vox", "body width, voxels"),
    ("bird.chest_at", m_voxels, "vox", "solid voxels"),
    ("bird.breast", m_voxels, "vox", "solid voxels"),
    ("bird.rump", m_voxels, "vox", "solid voxels"),
    ("bird.fullness", m_voxels, "vox", "solid voxels"),
    ("bird.belly", m_belly_share, "frac", "body mass above the outline mid"),
    ("bird.section", m_voxels, "vox", "solid voxels"),
    ("bird.neck_thick", m_voxels, "vox", "solid voxels"),
    ("bird.neck_up_deg", m_height, "vox", "height, voxels"),
    ("bird.head_size", m_head_diam, "vox", "head diameter, voxels"),
    ("bird.crest", m_crest, "vox", "crest above the head, voxels"),
    ("bird.bill_depth", m_bill_depth, "vox", "bill depth at the base, voxels"),
    ("bird.bill_curve", m_bill_bend, "vox", "bill bend, voxels"),
    ("bird.bill_hook", m_bill_drop, "vox", "bill tip drop, voxels"),
    ("bird.bill_gape", m_bill_width, "vox", "bill width, voxels"),
    ("bird.tail_width", m_tail_span, "vox", "tail span, voxels"),
    ("bird.tail_fork", m_tail_notch, "vox", "trailing edge unevenness, vox"),
    ("bird.tail_droop", m_height, "vox", "height, voxels"),
    ("bird.tail_thick", m_voxels, "vox", "solid voxels"),
    ("bird.wing_span", m_wing_reach, "vox", "reach past the body, voxels"),
    ("bird.wing_aspect", m_wing_chord, "vox", "chord at half span, voxels"),
    ("bird.wing_sweep", m_wing_sweep, "vox", "tip carried back, voxels"),
    ("bird.wing_dihedral", m_wing_rise, "vox", "tip carried up, voxels"),
    ("bird.wing_slots", m_wing_gaps, "vox", "runs across the wingtip"),
    ("bird.wing_thick", m_voxels, "vox", "solid voxels"),
    ("bird.wing_fold", m_wing_fold, "vox", "wingtip short of the tail, cols"),
    ("bird.leg_len", m_leg_drop, "vox", "legs below the belly, voxels"),
    ("bird.eye", m_eye, "vox", "eye voxels"),
    ("bird.upperparts", m_upper, "pct", "upperparts colour, % of bird"),
    ("bird.mark_count", m_mark_runs, "vox", "bands of marking"),
    ("bird.mark_width", m_mark, "pct", "marking, % of bird"),
    ("bird.mark_strength", m_mark, "pct", "marking, % of bird"),
]

# What a parameter needs SWITCHED ON before it can possibly do anything.
#
# Sweeping `wing_slots` on a perched bird measures nothing and reports DEAD --
# which is a false alarm, and a false alarm on a silent-no-op detector is worse
# than no detector at all, because it teaches you to skim the output. The fish
# probe shipped exactly that on its first run; this table is the fix, and the
# bird version needs a bigger one because a bird has two poses and three
# marking regions and most parameters are meaningless in one of them.
FLY = {"bird.pose": "flying", "bird.wing_span": 2.4, "bird.wing_thick": 2}
SETUP_FOR = {
    "bird.wing_span": {**FLY, "bird.wing_span": 2.4},
    "bird.wing_aspect": FLY,
    "bird.wing_sweep": FLY,
    "bird.wing_dihedral": FLY,
    "bird.wing_slots": {**FLY, "bird.wing_shape": "slotted"},
    "bird.wing_thick": FLY,
    # ... and the folded reach is the opposite: it exists ONLY when perched.
    "bird.wing_fold": {"bird.pose": "perched"},
    "bird.neck_frac": {"materials.bird_head": "plume_crimson"},
    "bird.neck_up_deg": {"materials.bird_head": "plume_crimson"},
    "bird.neck_thick": {"materials.bird_head": "plume_crimson"},
    # A fork is a difference between two feather lengths, so it needs a tail
    # long enough for the difference to be more than one voxel: on a 20 cm
    # robin the whole tail is six voxels and 0 vs 0.9 of fork moves the centre
    # by two of them, which is inside the noise of which voxel the rounding
    # picked.
    "bird.tail_fork": {"bird.tail_shape": "forked", "bird.tail_width": 0.9,
                       "bird.length_m": 0.9, "bird.tail_frac": 0.45},
    "bird.tail_width": {"bird.tail_shape": "square", "bird.length_m": 0.9},
    # ON A LONGER BIRD, and that is a finding rather than a convenience. A
    # 20 cm robin has a body eight voxels long; twenty-four bars across it is
    # a period of a third of a voxel, so 1 bar and 24 bars both come out as
    # "the whole body is marked" and the sweep reported the count slider DEAD.
    # It is not dead, it is SATURATED -- above about five bars on a
    # twenty-voxel bird they merge, which is the same 2-on-2-off floor the
    # fish work measured. The species that can carry many bars are the big
    # ones, so the sweep is run on one.
    "bird.mark_count": {"bird.body_mark": "barred", "bird.length_m": 0.9,
                        "materials.bird_mark": "plume_crimson"},
    "bird.mark_width": {"bird.body_mark": "barred", "bird.length_m": 0.9,
                        "materials.bird_mark": "plume_crimson"},
    "bird.mark_strength": {"bird.body_mark": "speckled",
                           "materials.bird_mark": "plume_crimson"},
    "bird.upperparts": {"materials.bird_back": "plume_crimson"},
    # The head and the bill are drawn in the species' own colours, and on most
    # species several parts share one. These force a colour nothing else uses,
    # so the histogram measures the part and not the paint scheme.
    "bird.head_frac": {"materials.bird_head": "plume_crimson"},
    "bird.head_size": {"materials.bird_head": "plume_crimson"},
    "bird.bill_frac": {"materials.bird_bill": "plume_crimson",
                       "bird.leg_len": 0.0},
    "bird.bill_depth": {"materials.bird_bill": "plume_crimson",
                        "bird.leg_len": 0.0, "bird.bill_frac": 0.20,
                        "bird.length_m": 0.9},
    # A bend is a departure from a straight line, so it needs a line long
    # enough to depart from: at 4 voxels of bill the whole range moves the
    # middle by half a voxel and the sweep called a working slider weak.
    "bird.bill_curve": {"materials.bird_bill": "plume_crimson",
                        "bird.leg_len": 0.0, "bird.bill_frac": 0.24,
                        "bird.length_m": 0.9},
    "bird.bill_hook": {"materials.bird_bill": "plume_crimson",
                       "bird.leg_len": 0.0, "bird.bill_frac": 0.24,
                       "bird.length_m": 0.9},
    "bird.bill_gape": {"materials.bird_bill": "plume_crimson",
                       "bird.leg_len": 0.0, "bird.bill_frac": 0.24,
                       "bird.length_m": 0.9},
    "bird.leg_len": {"materials.bird_bill": "plume_crimson",
                     "bird.pose": "perched"},
    "bird.crest": {"bird.head_frac": 0.16},
    "bird.eye": {"materials.bird_eye": "plume_lilac"},
}

# Choices are not numbers, so they are swept as a set rather than a range.
CHOICE_SWEEPS = [
    ("bird.tail_shape", _TAILS := ("square", "rounded", "graduated", "wedge",
                                   "notched", "forked", "pointed"),
     m_tail_notch, "vox", "trailing edge unevenness, voxels",
     {"bird.tail_fork": 0.80, "bird.tail_width": 0.9, "bird.length_m": 0.9,
      "bird.tail_frac": 0.45}),
    ("bird.wing_shape", ("elliptical", "pointed", "soaring", "slotted"),
     m_wing_taper, "vox", "chord lost between half and 85% span, voxels", FLY),
    ("bird.wing_shape", ("elliptical", "pointed", "soaring", "slotted"),
     m_wing_sweep, "vox", "tip carried back, voxels", FLY),
    ("bird.head_mark", ("none", "cap", "mask", "supercilium", "throat", "collar"),
     m_head_mark, "pct", "head marking, % of bird",
     {"materials.bird_head_mark": "plume_crimson", "bird.neck_frac": 0.12}),
    ("bird.wing_mark", ("none", "bar", "doublebar", "panel", "tip"),
     m_mark, "pct", "marking, % of bird",
     {"materials.bird_mark": "plume_crimson"}),
    ("bird.body_mark", ("none", "barred", "streaked", "speckled", "breastband"),
     m_mark, "pct", "marking, % of bird",
     {"materials.bird_mark": "plume_crimson"}),
    ("bird.pose", ("perched", "flying"), m_span, "vox", "span, voxels", {}),
]


def _mean(spec, measure, seeds) -> float:
    vals = []
    for s in seeds:
        try:
            vals.append(measure(pipeline.build(spec, s)))
        except Exception as exc:                       # noqa: BLE001
            print(f"      build failed at seed {s}: {type(exc).__name__}: {exc}")
    return float(np.mean(vals)) if vals else float("nan")


# Smallest change worth calling real, per unit of measurement. The one-voxel
# floor is the whole point of the exercise; applying it to a measurement whose
# units are a FRACTION would demand a change of 1.0, which no fraction can
# make, and the fish probe duly reported two working sliders as weak on
# exactly that basis.
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
    # Not to flatter the numbers -- to make them mean anything. Every bird spec
    # authors `variation.height` at 0.14, so two seeds of one species differ in
    # length by about two voxels of standard deviation, and a bill is two
    # voxels long. Averaged over four seeds a working bill slider sits inside
    # that noise and this tool would call it DEAD -- which is the false alarm
    # that makes a no-op detector worse than useless.
    #
    # Seeds are still averaged over, because with the size pinned the seed
    # still moves the speckle field, and that is exactly the parameter a single
    # seed would lie about. That variation ITSELF works is a separate test,
    # immediately below this one.
    base, _ = sm.patch(base, {"variation.amount": 0.0})
    print(f"\nPARAMETER SWEEPS on {sm.get(base, 'name')} "
          f"at {sm.get(base, 'resolution_cm')} cm, {len(seeds)} seeds "
          f"averaged, individual variation off\n")
    print(f"{'parameter':<24} {'measurement':<32} {'low':>9} {'high':>9} "
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
        print(f"{path:<24} {label:<32} {lo:>9.2f} {hi:>9.2f} "
              f"{rel * 100:>7.0f}%  {state}{note}")

    print()
    for path, choices, measure, unit, label, setup in CHOICE_SWEEPS:
        spec = sm.patch(base, setup)[0] if setup else base
        vals = []
        for c in choices:
            s, _ = sm.patch(spec, {path: c})
            vals.append((c, _mean(s, measure, seeds)))
        lo = min(v for _, v in vals)
        hi = max(v for _, v in vals)
        state, rel = _verdict(lo, hi, unit)
        dead += state in ("DEAD", "ERROR")
        detail = "  ".join(f"{c}={v:.1f}" for c, v in vals)
        print(f"{path:<24} {label:<32} {rel * 100:>7.0f}%  {state}")
        print(f"{'':<24} {detail}")
    return dead


def variation(base: dict, seeds: list[int]) -> None:
    """Do two individuals of one species actually differ?

    The failure this catches is specific and has happened here before: a bird
    reads `variation.*` in `bird._params` while `spec.realize` varies the tree
    parameters, so a wiring mistake shows up as a flock of identical animals --
    and a flock is the one place that is unmissable and the one place nobody
    looks until it is in the game.
    """
    print("\nVARIATION across seeds (a flock of clones is the failure)\n")
    for amount in (0.0, 1.0, 2.0):
        spec, _ = sm.patch(base, {"variation.amount": amount})
        lens = [m_length(pipeline.build(spec, s)) for s in seeds]
        deps = [m_body_depth(pipeline.build(spec, s)) for s in seeds]
        tails = [m_tail_run(pipeline.build(spec, s)) for s in seeds]
        print(f"  variation.amount {amount:>4.1f}   "
              f"length {np.mean(lens):>5.1f} +/- {np.std(lens):>4.2f}   "
              f"body depth {np.mean(deps):>4.1f} +/- {np.std(deps):>4.2f}   "
              f"tail {np.mean(tails):>4.1f} +/- {np.std(tails):>4.2f}   "
              f"distinct lengths {len(set(lens))}/{len(lens)}")


# --- readability ------------------------------------------------------------


def _luminance(rgb) -> float:
    def lin(c: float) -> float:
        c = c / 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4

    r, g, b = (lin(v) for v in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def _contrast(c1, c2) -> float:
    a, b = sorted((_luminance(c1), _luminance(c2)), reverse=True)
    return (a + 0.05) / (b + 0.05)


# The floor a marking has to clear against what it sits on.
#
# The fish probe uses 1.5 and this uses 2.0, and the difference is the brief:
# "colourful and stylised". At 1.5 a marking is present and faint; at 2.0 it is
# a block of colour. It is also above the level where Mojang's own tropical
# fish stop reading -- 16% of their random dye pairs come out under a contrast
# ratio of 1.2 and one shipped preset at 67% ink measures 1.04, which genuinely
# renders as a plain blue blob.
CONTRAST_FLOOR = 2.0


def _ink_delta(marked, plain) -> float:
    """Share of the bird whose material CHANGED when the markings were turned
    on. See the comment in `readability`."""
    a, b = marked.stats["by_material"], plain.stats["by_material"]
    total = max(sum(a.values()), 1)
    gained = sum(max(0, a.get(k, 0) - b.get(k, 0)) for k in set(a) | set(b))
    return 100.0 * gained / total


def readability(names: list[str], seeds: list[int]) -> int:
    """Silhouette, value contrast, and survival of a one-voxel erosion.

    WHY VALUE AND NOT COLOUR. The eye carries brightness at about four times
    the spatial detail it carries red-against-green, which is why video has
    thrown away colour resolution for fifty years and nobody notices. A marking
    that differs from what it sits on only in HUE blurs away roughly four times
    sooner than one that differs in BRIGHTNESS -- and at twenty-five voxels,
    "four times sooner" means "before the player ever sees it".

    THREE CONTRASTS, NOT ONE, because a bird has three marking regions and they
    sit on three different colours. The wing mark is checked against the wing,
    the body mark against the underparts and the head mark against the head. A
    single check against a single base colour passed every species in the
    library and would have shipped a white wing bar on a white ptarmigan.
    """
    print("\nREADABILITY  (silhouette, value contrast, erosion survival)\n")
    print(f"{'species':<26} {'len':>4} {'tall':>5} {'ink%':>6} {'wing':>6} "
          f"{'body':>6} {'head':>6} {'bill':>6} {'keeps':>7}  flags")
    bad = 0
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        # PINNED. With individual variation on, seed 1 of a species is two
        # voxels longer or shorter depending on what the spec hash happened to
        # be, and the SHORT flag flickered on and off across edits that had
        # nothing to do with size. This table is a gate on the SPECIES.
        spec, _ = sm.patch(spec, {"variation.amount": 0.0})
        a = pipeline.build(spec, seeds[0])
        occ = _occ(a)
        # THE LONGEST AXIS, not the x axis. A great spotted woodpecker is
        # authored at 68 degrees nose-up, so 23 cm of bird projects onto 12
        # voxels of x and 16 of z -- and the flag fired SHORT on a species
        # that is the right size and merely steep. Every other measurement in
        # this file is happy to be axis-aligned; this one decides whether a
        # species gets re-authored, so it reads the animal rather than the box.
        length, tall = m_length(a), m_height(a)
        reach = max(length, tall)
        # INK IS MEASURED AGAINST THE SAME BIRD WITH ITS MARKINGS TURNED OFF,
        # rather than by counting the marking colour. Counting it cannot work
        # here: a woodpecker's white wing panel and its white underparts are
        # the same material, so either the belly is counted as marking (which
        # reported a herring gull at 74% ink on a species with no marking at
        # all) or the panel is discarded with it (which reported the woodpecker
        # at 1.0% and flagged a species whose panel is one of the boldest in
        # the library). Subtracting a baseline build measures the marking and
        # only the marking, whatever colour it happens to share.
        # BOTH BUILDS WITH VARIATION OFF. Turning a marking off changes the
        # spec hash, `pipeline.rng_for` mixes the hash into the seed, and the
        # two builds are then two different individuals of different sizes --
        # a buzzard came back with 9,899 brown voxels unmarked against 5,556
        # marked, so the difference was dominated by the bird being smaller
        # and the ink measured as zero on a species with a bold wing panel.
        pinned, _ = sm.patch(spec, {"variation.amount": 0.0})
        plain, _ = sm.patch(pinned, {"bird.wing_mark": "none",
                                     "bird.body_mark": "none",
                                     "bird.head_mark": "none"})
        ink = _ink_delta(pipeline.build(pinned, seeds[0]),
                         pipeline.build(plain, seeds[0]))

        # THROUGH `_bird_mat`, NOT OFF THE SPEC. This table runs on the spec as
        # authored, where the two are the same thing -- but the day somebody
        # points it at a sexed bird, reading the spec would grade the drake's
        # colours and print the hen's name. `sexes` checks the other sex
        # separately; see `_alt_contrast` for why that check had to exist.
        col = materials.color
        wing = col(_bird_mat(spec, "wing"))
        under = col(_bird_mat(spec, "belly"))
        head = col(_bird_mat(spec, "head"))
        mark = col(_bird_mat(spec, "mark"))
        hmark = col(_bird_mat(spec, "head_mark"))

        cw = (_contrast(wing, mark)
              if _bird_mark(spec, "wing_mark") != "none" else float("nan"))
        cb = (_contrast(under, mark)
              if _bird_mark(spec, "body_mark") != "none" else float("nan"))
        ch = (_contrast(head, hmark)
              if _bird_mark(spec, "head_mark") != "none" else float("nan"))
        # THE BILL AGAINST THE HEAD, which is a fourth check and was not here
        # at first. It should have been: *Pixel Logic* records that Super Mario
        # World's Swoopers are bats that read as birds purely because their
        # nose was coloured orange, and Minecraft, Mega Voxels and every 16x16
        # sprite pack examined all give the beak its own hue. A bill is two to
        # sixteen voxels and it is the cheapest identifying feature a bird has
        # -- and when this check was finally added, TEN OF THE TWENTY SPECIES
        # were drawing it in a colour that did not separate from the head it
        # sticks out of. A robin's horn bill on its olive head measured 1.04,
        # which is no bill at all.
        cbill = _contrast(col(_bird_mat(spec, "bill")), head)

        # Erosion survival: does the SILHOUETTE still have a bird's shape after
        # the equivalent of moving a voxel away? The pixel-art "features under
        # two across disappear" rule, run rather than quoted.
        sil = occ.any(axis=1)
        keep = sil.copy()
        k = keep.copy()
        k[:-1] &= keep[1:]
        k[1:] &= keep[:-1]
        k[:, :-1] &= keep[:, 1:]
        k[:, 1:] &= keep[:, :-1]
        survive = 100.0 * k.sum() / max(sil.sum(), 1)

        flags = []
        if reach < 18:
            flags.append(f"SHORT ({reach:.0f} vox; the reference is 20-40)")
        marked = sum(_bird_mark(spec, f"{r}_mark") != "none"
                     for r in ("wing", "body", "head"))
        if marked and ink < 2.5:
            flags.append(f"FAINT MARKING ({ink:.1f}% of the bird)")
        for what, cr in (("wing", cw), ("body", cb), ("head", ch),
                         ("bill", cbill)):
            if not np.isnan(cr) and cr < CONTRAST_FLOOR:
                flags.append(f"LOW CONTRAST on the {what} "
                             f"({cr:.2f}, floor is {CONTRAST_FLOOR})")
        if survive < 20:
            flags.append(f"THIN ({survive:.0f}% survives a 1-voxel erosion)")
        bad += bool(flags)

        def f(v):
            return "    -" if np.isnan(v) else f"{v:>5.2f}"

        print(f"{n:<26} {length:>4.0f} {tall:>5.0f} {ink:>6.1f} {f(cw):>6} "
              f"{f(cb):>6} {f(ch):>6} {f(cbill):>6} {survive:>6.0f}%  "
              f"{'; '.join(flags)}")
    return bad


def lattice(names: list[str], seeds: list[int]) -> None:
    """1 cm against 2 cm and 5 cm, on the features that decide it.

    The lattice argument is not about cost -- a bird is free at any of these
    sizes -- it is entirely about whether the FEATURES still exist. So this
    measures the features, not the voxel count.
    """
    print("\nLATTICE  (does the bird survive a coarser voxel?)\n")
    print(f"{'species':<26} {'cm':>4} {'len':>4} {'bill':>5} {'eye':>4} "
          f"{'tail':>5} {'neck':>5} {'crest':>6} {'vox':>8}")
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        # UNIQUE COLOURS FOR THE EYE AND THE BILL, and variation pinned. The
        # question here is "does the feature survive a coarser voxel", not
        # "what colour is it" -- and nine of the twenty species paint their eye
        # in a colour something else on the bird also wears, which made the eye
        # column unreadable on exactly the species it matters most for.
        spec, _ = sm.patch(spec, {"variation.amount": 0.0,
                                  "materials.bird_eye": "plume_lilac",
                                  "materials.bird_bill": "plume_crimson"})
        for cm in (1.0, 2.0, 5.0):
            a = pipeline.build(spec, seeds[0], resolution_cm=cm)
            print(f"{n if cm == 1.0 else '':<26} {cm:>4.0f} "
                  f"{m_length(a):>4.0f} {m_bill_run(a):>5.0f} "
                  f"{('  -' if np.isnan(m_eye(a)) else f'{m_eye(a):>3.0f}'):>4} "
                  f"{m_tail_run(a):>5.0f} {m_neck_run(a):>5.1f} "
                  f"{m_crest(a):>6.1f} {a.stats['voxels']:>8,}")


def pose(names: list[str], seeds: list[int]) -> int:
    """Every species in both poses: does it build, and is it the same bird?

    Two tables, and they answer two different questions.

    THE FIRST IS GEOMETRY. Spreading the wings should multiply the SPAN by
    three or four and leave the LENGTH alone -- measured on avian-mesh's two
    published rest poses, span 7.17 -> 32.31 (x4.51) against a body length of
    20.06 -> 20.86 (x1.04). If the length moves here, the pose is doing
    something to the body it should not be; if the span does not, the wings are
    not being drawn. It also counts PIECES at 26-connectivity in both poses on
    every seed, because "all twenty species are authorable in both poses" is a
    claim about whether the assets are shippable and not only about whether the
    build returns. The failure to expect is a spread wing coming away at the
    shoulder.

    THE SECOND IS THE ONE THAT MATTERS NOW, and it is new. A pose is a posture,
    not a different animal. Until `spec.seed_hash` existed, `pipeline.rng_for`
    seeded from `spec.spec_hash`, the pose was part of that hash, and
    `common-raven` seed 7 perched and `common-raven` seed 7 flying were two
    different ravens -- different size, different markings. A bird could not
    land without changing on the way down.

    Returns the number of species that failed either table.
    """
    fails = 0
    print("\nPOSE  (folded against spread, on every species)\n")
    print(f"{'species':<26} {'len P':>6} {'len F':>6} {'x':>5} "
          f"{'span P':>7} {'span F':>7} {'x':>5} {'vox P':>7} {'vox F':>7}"
          f"  {'pieces':>8}")
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        p, _ = sm.patch(spec, {"bird.pose": "perched", "variation.amount": 0.0})
        f, _ = sm.patch(spec, {"bird.pose": "flying", "variation.amount": 0.0})
        ap = pipeline.build(p, seeds[0])
        af = pipeline.build(f, seeds[0])
        lp, lf = m_length(ap), m_length(af)
        sp, sf = m_span(ap), m_span(af)
        # ONE PIECE, IN BOTH POSES, ON EVERY SEED ASKED FOR. `tools/buildcheck.py`
        # only ever builds a species in its AUTHORED pose, so until this line
        # existed the other pose of fifteen species had never been checked at
        # all -- and the other pose is exactly where a wing is a one-voxel plate
        # instead of a rind on the flank.
        broken = []
        for tag in ("perched", "flying"):
            # Variation ON here, unlike the columns above: the size a seed draws
            # is what decides whether a thin part survives, and a check run only
            # at the species average is a check that misses the small
            # individuals.
            live, _ = sm.patch(spec, {"bird.pose": tag})
            for s in seeds:
                n_pc, sizes = cli.pieces(pipeline.build(live, s))
                if n_pc > 1:
                    broken.append(f"{tag} seed {s}: {n_pc} pieces, "
                                  f"largest loose {sizes[1]:,}")
        fails += bool(broken)
        print(f"{n:<26} {lp:>6.0f} {lf:>6.0f} {lf / max(lp, 1):>5.2f} "
              f"{sp:>7.0f} {sf:>7.0f} {sf / max(sp, 1):>5.2f} "
              f"{ap.stats['voxels']:>7,} {af.stats['voxels']:>7,}"
              f"  {'ok' if not broken else 'BROKEN':>8}")
        for line in broken:
            print(f"{'':<26}   ! {line}")

    fails += _same_individual(names, seeds)
    return fails


# Colours to force onto a part when a measurement has to see that part and
# nothing else. Any of them will do; which one is picked per species is
# whichever the species is not already wearing somewhere.
_SPARE_COLOURS = ("plume_crimson", "plume_lilac", "plume_cyan", "plume_lime",
                  "plume_iridescent", "skin_red", "skin_green")

_BIRD_SLOTS = ("back", "belly", "head", "wing", "mark", "head_mark", "bill",
               "eye")


def _spare_colours(spec: dict, want: int) -> list[str]:
    """`want` materials this species wears nowhere else.

    Picked rather than hardcoded. A hardcoded `plume_crimson` is right on
    nineteen species and silently wrong on the twentieth -- the scarlet macaw
    already wears it, so a measurement keyed to it would count the macaw's whole
    body and report a head the size of a bird.
    """
    # THE `alt` COLOURS COUNT AS USED TOO. A colour the species wears only as
    # the other sex is still a colour a histogram cannot tell apart from the
    # part being measured, and the whole point of this function is that a
    # hardcoded `plume_crimson` is right on nineteen species and silently wrong
    # on the twentieth.
    used = {sm.get(spec, f"materials.bird_{k}") for k in _BIRD_SLOTS}
    used |= {sm.get(spec, f"materials.bird_alt_{k}")
             for k in _BIRD_SLOTS if k != "eye"}
    free = [c for c in _SPARE_COLOURS if c not in used]
    if len(free) < want:
        raise SystemExit(
            f"{sm.get(spec, 'name')} wears too much of the palette for this "
            f"measurement: wanted {want} spare colours, found {len(free)}")
    return free[:want]


# The numbers `bird._params` varies that the POSE does not touch. This is what
# "the same individual" means: the length this seed drew, how that length was
# divided between the five parts, how deep and how wide the body came out, and
# the two random values the markings are drawn from.
#
# `posture` is NOT in the list and cannot be -- a flying bird's body lies along
# its line of travel, so `bird._params` deliberately scales the posture by 0.35
# when the pose is `flying`. That is the pose doing its job.
_INDIVIDUAL_KEYS = ("length_v", "body_v", "tail_v", "neck_v", "bill_v",
                    "head_r", "depth_v", "width_v", "phase", "salt")


def _individual(spec: dict, seed: int) -> dict:
    """The individual this (spec, seed) draws, as the numbers the drawing code
    was handed."""
    from forge import bird as birdlib

    p = birdlib._params(spec, pipeline.rng_for(spec, seed),
                        float(sm.get(spec, "resolution_cm")) / 100.0)
    return {k: p[k] for k in _INDIVIDUAL_KEYS}


def _same_individual(names: list[str], seeds: list[int]) -> int:
    """Is species X seed N the same bird in both poses? Measured three ways.

    THE OWNER'S ASK WAS "different poses should not be different generated
    birds". Until `spec.seed_hash` existed they were: the pose is part of the
    spec, `pipeline.rng_for` mixed the spec hash into the seed, and a different
    seed draws a different length, different shares and a different marking
    phase. This is the test that says whether that is fixed, and it is written
    so that it FAILS if the fix is reverted.

    1. THE SEED ITSELF. `spec.seed_hash` must return the same digest for the two
       poses. Exact, and it is the fix stated directly.

    2. THE INDIVIDUAL THE DRAWING CODE WAS HANDED. `_INDIVIDUAL_KEYS` off
       `bird._params` -- the varied length, the five varied shares, the body's
       depth and width, and the marking phase and salt. Exact. This is an
       INTERNAL read, which this file otherwise refuses to do, and the reason it
       is allowed here is that the sweep table above has already proved every
       one of these numbers reaches the voxels: `bird.length_m` moves
       `m_length`, `bird.tail_frac` moves `m_tail_run`, `bird.head_frac` moves
       `m_head_diam`. What is being tested here is not whether they reach the
       voxels but whether the two poses got the same ones.

    3. THE VOXELS, ON THE BILL AND ONLY ON THE BILL. This is the honest part and
       it is worth stating why it is so narrow. The two poses do NOT rasterise
       on the same lattice: `bird._params` sizes the grid from the layout's own
       bounding box, a perched bird has legs hanging below it and a flying one
       has wings reaching out sideways, so the grid's y size and therefore its
       mid-plane differ. A different mid-plane is a different sub-voxel offset,
       and on a three-voxel head that is worth a whole voxel of measured
       diameter for reasons that have nothing to do with which bird it is.
       Measured: the head's diameter agrees across poses on 19 of the 20
       species and differs by one voxel on `barn-swallow`, and the animal's own
       length agrees on 19 and differs by four on `barn-swallow` -- whose
       folded primaries reach further aft than its tail does, which is a real
       difference between the two poses and not a different bird.

       The bill is the one part the pose touches neither directly nor through
       the grid's y origin: it is drawn forward of the head, on the mid-plane,
       from `bill_v` and `head_r`. Its length in columns and its depth at the
       base agree EXACTLY across poses on all twenty species at both seeds, so
       they are what the verdict is taken on. Four fields are still pinned --
       posture, leg length, wing sweep and dihedral -- because those four move
       the box in x and z as well, and the bill would move with it.

    AND ALL THREE CARRY A CONTROL, because a measurement that cannot fail is
    worse than none. Every number is taken at TWO seeds, and the table reports
    how many species come back different between them. If that count is zero the
    numbers are not reading the individual at all, the pose agreement proves
    nothing, and this returns a failure for every species rather than a clean
    table -- which is exactly the reassuring lie this file exists to catch.
    """
    print("\nSAME INDIVIDUAL IN BOTH POSES  (a pose is a posture, not a bird)\n")
    print("  P = perched, F = flying, same species, same seed. Layout pinned")
    print("  (posture, legs, sweep, dihedral) and the bill forced to a colour")
    print("  the species wears nowhere else; individual variation ON.\n")
    print(f"{'species':<26} {'seed':>5} {'seed hash':>10} {'len_v':>8} "
          f"{'phase':>8} {'bill P':>7} {'bill F':>7} {'deep P':>7} {'deep F':>7}"
          f"  verdict")
    pinned = {"bird.posture_deg": 0.0, "bird.leg_len": 0.0,
              "bird.wing_sweep": 0.0, "bird.wing_dihedral": 0.0}
    two = seeds[:2] if len(seeds) >= 2 else [seeds[0], seeds[0] + 1]
    bad = 0
    moved_ind = moved_vox = 0
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        (bill_c,) = _spare_colours(spec, 1)
        spec, _ = sm.patch(spec, {**pinned, "materials.bird_bill": bill_c})
        p, _ = sm.patch(spec, {"bird.pose": "perched"})
        f, _ = sm.patch(spec, {"bird.pose": "flying"})
        hp, hf = sm.seed_hash(p), sm.seed_hash(f)
        rows, ok = [], (hp == hf)
        for s in two:
            ip, if_ = _individual(p, s), _individual(f, s)
            ap, af = pipeline.build(p, s), pipeline.build(f, s)
            vox = (m_bill_run(ap), m_bill_run(af),
                   m_bill_depth(ap), m_bill_depth(af))
            ok &= (ip == if_) and vox[0] == vox[1] and vox[2] == vox[3]
            rows.append((s, ip, if_, vox))
        bad += not ok
        moved_ind += rows[0][1] != rows[1][1]
        moved_vox += (rows[0][3][0], rows[0][3][2]) != (rows[1][3][0], rows[1][3][2])
        for i, (s, ip, if_, vox) in enumerate(rows):
            agree = "same" if ip == if_ else "DIFFER"
            print(f"{n if i == 0 else '':<26} {s:>5} "
                  f"{(hp[:8] if hp == hf else 'DIFFER'):>10} "
                  f"{ip['length_v']:>8.2f} {ip['phase']:>8.4f} "
                  f"{vox[0]:>7.0f} {vox[1]:>7.0f} {vox[2]:>7.0f} {vox[3]:>7.0f}"
                  f"  {agree if i or ok else 'DIFFERENT BIRD'}")
    # The seed hash is per SPEC, so it does not move between seeds and is not
    # part of the control. The two that are: the individual's own varied
    # numbers, and the bill in voxels.
    print(f"\n  {len(names) - bad}/{len(names)} species draw ONE individual in "
          f"both poses (seed hash, {len(_INDIVIDUAL_KEYS)} varied numbers, and "
          f"the bill in voxels).")
    print(f"  control, seed {two[0]} against seed {two[1]}: the varied numbers "
          f"moved on {moved_ind}/{len(names)} species and the bill in voxels "
          f"moved on {moved_vox}/{len(names)}.")
    if not moved_ind or not moved_vox:
        print("  ! a number that is the same for two different birds cannot "
              "prove two poses are one bird -- the agreement above proves "
              "NOTHING")
        return len(names)
    return bad


# --- sex --------------------------------------------------------------------


def _shares(a) -> dict[int, float]:
    """The bird's material histogram as fractions of the bird."""
    hist = a.stats["by_material"]
    total = max(sum(hist.values()), 1)
    return {k: v / total for k, v in hist.items()}


def m_recolour(a, b) -> float:
    """How much of the bird changed colour between two builds, in % of it.

    MEASURED AS THE DISTANCE BETWEEN TWO MATERIAL HISTOGRAMS, IN SHARES, and
    both halves of that are forced on us rather than chosen.

    NOT VOXEL BY VOXEL, because the two builds are not the same individual and
    cannot be made to be. Sex reseeds on purpose -- that is the design decision
    `bird.sex` states -- so a male and a female of one seed draw different
    marking phases even with `variation.amount` pinned to zero, and a
    voxel-wise diff would report a speckled starling as heavily recoloured on
    the strength of its speckles having moved. The histogram is blind to WHERE
    the paint went, which is exactly the property wanted here.

    IN SHARES RATHER THAN COUNTS, because a species that also carries a size
    ratio draws two birds of different volumes, and a raw count difference on a
    golden eagle would report a third of the animal recoloured when nothing was
    repainted at all -- there is simply more of her.

    THE NOISE FLOOR IS NOT ASSUMED, IT IS MEASURED, AND IT IS NOT SMALL. `sexes`
    prints a control column beside this one: the same figure between two SEEDS
    of one sex, which is the marking phase moving and nothing else. On a
    song thrush it runs to eight per cent, and the reason is worth writing
    down because it looks like a bug and is not. The thrush is 314 voxels. Its
    speckling is placed as a QUANTILE of a noise field, so the number of
    speckled voxels is identical between any two individuals -- but WHICH
    voxels, and therefore whether a speckle lands on the rufous back or the
    white belly, is not. Thirty-nine voxels of speckle change which region they
    sit on, the two region colours swap thirty-odd voxels between them, and the
    histogram moves by nine per cent without a single decision differing.

    So a plumage swap has to clear its own control by a wide margin, and the
    flag in `sexes` is written against the control rather than against a
    constant.
    """
    sa, sb = _shares(a), _shares(b)
    keys = set(sa) | set(sb)
    return 50.0 * sum(abs(sa.get(k, 0.0) - sb.get(k, 0.0)) for k in keys)


# Which of the ten `alt` rows a species has actually filled in. Read off the
# spec rather than off the build, so it is exact and says nothing about voxels
# -- the voxels are what `m_recolour` is for.
#
# IMPORTED FROM `forge/bird.py` RATHER THAN RETYPED. Ten strings that have to
# stay equal to ten strings in another file is the drift `forge/materials.py`
# warns about in its own header, and a probe holding a stale copy of the list
# would report "10/10 alt rows reach the voxels" while never testing the
# eleventh.
_ALT_MAT_SLOTS = _birdlib.ALT_SLOTS
_ALT_MARK_SLOTS = _birdlib.ALT_MARKS


def _alt_rows(spec: dict) -> list[str]:
    rows = [f"{s}={sm.get(spec, f'materials.bird_alt_{s}')}"
            for s in _ALT_MAT_SLOTS
            if sm.get(spec, f"materials.bird_alt_{s}") != "same"]
    rows += [f"{s}={sm.get(spec, f'bird.sex_alt_{s}')}"
             for s in _ALT_MARK_SLOTS
             if sm.get(spec, f"bird.sex_alt_{s}") != "same"]
    return rows


def sexes(names: list[str], seeds: list[int]) -> int:
    """Male against female, per species, in voxels and in repainted area.

    THE POINT OF THIS TABLE IS THE SPECIES THAT DO NOT MOVE. Most of this
    library has no difference worth drawing at these sizes and the honest
    output is a row of zeros with the ratios at 1.00 and the plumage at
    `same` -- not an invented difference. What it has to catch is the other
    case: a species that AUTHORS a difference and does not get it, which is the
    silent no-op wearing a field guide's jacket.

    TWO MECHANISMS, SO TWO SETS OF NUMBERS, and the reason there are two is the
    whole difference between this file and the fish one. A fish's sexual
    difference is size and three ratios covered it. A bird's is nearly all
    plumage: a mallard drake against a hen is a bottle-green head, a white
    collar and a grey body against uniform mottled brown, and no ratio
    anywhere can say that. So:

      * THE RATIOS are checked the way the fish's are -- the measured
        male-to-female difference in voxels has to clear two on any species
        that claims a ratio at all, and `unsexed` has to sit between the two
        sexes because the authored number is the mean and the ratio is split
        as a square root either way.

      * THE PLUMAGE SWAP is checked as REPAINTED AREA, in per cent of the bird,
        with a measured control beside it. See `m_recolour`.

      * AND THE TAIL RATIO IS CHECKED TWICE, once on the tail and once on the
        bill. `bird.tail_frac` is one of five shares normalised to sum to one,
        so the naive implementation lengthens the tail by shortening the head
        and the bill; the bill column is there to prove it did not.

      * A MALE AND A FEMALE OF ONE SEED ARE DIFFERENT INDIVIDUALS. That is the
        design decision and it is the opposite of `bird.pose`, so it is
        measured rather than assumed -- and it is measured on every species,
        including the twelve that are otherwise identical, because that is the
        only thing that separates "no dimorphism" from "the parameter is not
        wired up".

      * AND AN `alt` ROW ON A SPECIES DECLARED `same` IS A FAULT, not a spare.
        It is authored plumage that no setting of any parameter can ever draw,
        which is how a species gets "fixed" in a way that does nothing.
    """
    print("\nSEX  (male against female, individual variation off)\n")
    print("  ratios are male:female and are split as a square root either way,")
    print("  so `unsexed` is the geometric mean. PLUMAGE cannot be averaged: a")
    print("  dimorphic species is authored as ONE sex and `unsexed` draws that")
    print("  one. The `authored` column says which.\n")
    print(f"{'species':<26} {'L':>5} {'T':>5} {'authored':<9} {'alt':>4} "
          f"{'len f/m':>11} {'tail f/m':>11} {'bill f/m':>9} "
          f"{'repaint':>8} {'vox':>6} {'phase':>6}  flags")
    bad = 0
    n_dimorphic = n_flat = 0
    unsexed_is_a_sex = []
    # Two seeds are enough for the geometry, which is deterministic once
    # variation is off, and not nearly enough for the repaint figure, which is
    # a difference of two noisy quantities. Both columns are averaged over
    # every seed asked for.
    pairs = list(zip(seeds, seeds[1:])) or [(seeds[0], seeds[0] + 1)]
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        # PERCHED, ON EVERY SPECIES, INCLUDING THE FIVE AUTHORED FLYING, and
        # this is not tidiness -- it is the fix the barn swallow forced. A
        # flying bird's length along the grid's x axis is set by its SWEPT
        # WINGTIPS, not by its tail: a swallow's tips are carried ten voxels
        # aft and its whole tail is six, so a 20% tail ratio measured as one
        # voxel of length and `m_tail_run` came back 3, 6, 5 for female,
        # unsexed and male -- non-monotonic, which is a measurement reporting
        # noise as a result. Perched, the same ratio measures 11 against 13
        # columns of tail and the length 26 against 28, monotonic on both. The
        # pose is a posture and all twenty species are authorable in either, so
        # pinning it costs nothing and is what `SETUP_FOR` does for half the
        # sweep table already.
        pinned, _ = sm.patch(spec, {"variation.amount": 0.0,
                                    "bird.pose": "perched"})
        rl = float(sm.get(spec, "bird.sex_length"))
        rt = float(sm.get(spec, "bird.sex_tail"))
        authored = str(sm.get(spec, "bird.sex_plumage"))
        by_sex = {s: sm.patch(pinned, {"bird.sex": s})[0]
                  for s in ("unsexed", "female", "male")}
        built = {s: pipeline.build(v, seeds[0]) for s, v in by_sex.items()}
        L = {k: m_length(v) for k, v in built.items()}
        T = {k: m_tail_run(v) for k, v in built.items()}
        B = {k: m_bill_run(v) for k, v in built.items()}
        moved = max(abs(L["male"] - L["female"]), abs(T["male"] - T["female"]))
        repaint = float(np.mean([
            m_recolour(pipeline.build(by_sex["male"], s),
                       pipeline.build(by_sex["female"], s)) for s in seeds]))
        # THE CONTROL, and the table is unreadable without it. Two seeds of ONE
        # sex, same spec, variation off: everything that differs between them
        # is the marking phase, which is the noise `repaint` sits on top of.
        control = float(np.mean([
            m_recolour(pipeline.build(by_sex["unsexed"], i),
                       pipeline.build(by_sex["unsexed"], j)) for i, j in pairs]))

        claims_size = max(abs(rl - 1.0), abs(rt - 1.0)) > 0.005
        rows = _alt_rows(spec)
        claims_plumage = authored != "same"
        # The same figure in voxels. A PER CENT ALONE CANNOT GATE THIS and the
        # great spotted woodpecker is why: the entire published difference
        # between the sexes of that species is that the male carries a crimson
        # patch on the nape and the female does not, and at 1 cm that patch is
        # FOUR VOXELS on a 352-voxel bird -- 1.1%. A five-per-cent gate calls
        # the most-quoted field mark in the set a no-op. A two-voxel gate alone
        # is no better in the other direction: two voxels on a 28,355-voxel
        # golden eagle is a rounding error. Both, therefore.
        repaint_vox = repaint * 0.01 * np.mean(
            [built["male"].stats["voxels"], built["female"].stats["voxels"]])
        flags = []
        if claims_size and moved < 2.0:
            flags.append(f"CLAIMS A RATIO AND MOVES {moved:.0f} VOXELS")
        if not claims_size and moved >= 2.0:
            flags.append(f"NO RATIO AUTHORED BUT MOVED {moved:.0f} VOXELS")
        # The tail ratio must not be paid for out of the bill. Half a voxel is
        # the rounding of one column; anything above that is the normalisation
        # eating the rest of the bird.
        if abs(rt - 1.0) > 0.005 and abs(B["male"] - B["female"]) > 0.5:
            flags.append(f"the tail ratio moved the BILL by "
                         f"{abs(B['male'] - B['female']):.0f} columns")
        for label, m in (("length", L), ("tail", T)):
            lo, hi = sorted((m["female"], m["male"]))
            if hi - lo >= 2.0 and not (lo - 1.0 <= m["unsexed"] <= hi + 1.0):
                flags.append(f"unsexed {label} {m['unsexed']:.0f} is outside "
                             f"[{lo:.0f}, {hi:.0f}]")
        if claims_plumage and not rows:
            flags.append("declares a sexed plumage and fills in no alt row")
        # AGAINST THE SPECIES' OWN NOISE, not against a constant. A swap on a
        # thrush has to beat the six per cent its speckles move on their own;
        # a swap on a raven has to beat nothing, because a raven has no
        # speckles to move. And it has to be two voxels, whatever the per cent
        # says.
        floor = max(1.5 * control, 0.5)
        if claims_plumage and (repaint < floor or repaint_vox < 2.0):
            flags.append(f"CLAIMS A PLUMAGE SWAP AND REPAINTS {repaint:.1f}% "
                         f"= {repaint_vox:.0f} voxels, against a phase floor of "
                         f"{floor:.1f}% and a two-voxel floor")
        if not claims_plumage and rows:
            flags.append(f"sex_plumage=same but {len(rows)} alt row(s) are "
                         f"authored and can never be drawn: {', '.join(rows)}")
        # SEX RESEEDS. Checked on every species, not only the dimorphic ones:
        # on a monomorphic bird it is the ONLY thing that separates "male and
        # female look alike" from "the parameter never reached the build".
        hm = sm.seed_hash(sm.patch(spec, {"bird.sex": "male"})[0])
        hf = sm.seed_hash(sm.patch(spec, {"bird.sex": "female"})[0])
        if hm == hf:
            flags.append("male and female seed to the SAME individual; sex is "
                         "meant to reseed -- see bird.sex")
        if claims_size or claims_plumage:
            n_dimorphic += 1
        else:
            n_flat += 1
        if claims_plumage:
            unsexed_is_a_sex.append(f"{n} ({authored})")
        bad += bool(flags)
        print(f"{n:<26} {rl:>5.2f} {rt:>5.2f} {authored:<9} {len(rows):>4} "
              f"{L['female']:>5.0f}/{L['male']:<5.0f} "
              f"{T['female']:>5.0f}/{T['male']:<5.0f} "
              f"{B['female']:>4.0f}/{B['male']:<4.0f} "
              f"{repaint:>7.1f}% {repaint_vox:>6.0f} {control:>5.1f}%  "
              f"{'; '.join(flags)}")

    # SAY "NO GEOMETRY", NOT "NOTHING", because the repaint column beside it is
    # not zero on three of these and a summary line claiming it was would be
    # contradicted by the table directly above it. A song thrush repaints 6% of
    # its histogram between a male and a female that differ in no authored value
    # at all; that is its speckle phase, the control column measures it, and
    # rounding it away in the summary is how a true statement becomes a lie.
    print(f"\n  {n_dimorphic} of {len(names)} species carry a difference; "
          f"{n_flat} are authored at 1.00/1.00 and `same` and move NO GEOMETRY "
          f"at all -- the honest null. Any repaint on those is the marking "
          f"phase, and the `phase` column beside it is what that costs.")
    if unsexed_is_a_sex:
        print("\n  UNSEXED IS ONE OF THE SEXES on these, because colour has no "
              "average:")
        for line in unsexed_is_a_sex:
            print(f"    {line}")
    bad += _alt_contrast(names)
    bad += _alt_slots_live(seeds)
    return bad


def _alt_contrast(names: list[str]) -> int:
    """Does the OTHER sex's plumage clear the same contrast floor?

    A HOLE THE MOMENT THE SWAP EXISTED, and worth naming because it is the
    exact shape of this project's favourite bug. `readability` reads
    `materials.bird_*` directly, so it has always checked the plumage a spec is
    authored in and nothing else. The day a mallard grew a female, half the
    library's colour decisions stopped being covered by the gate that exists to
    cover them -- the hen is brown upperparts over buff underparts with a
    brown wing behind a blue speculum, and nothing anywhere asked whether those
    separate. The gate reported PASS on twenty species and was checking sixteen.

    THE EFFECTIVE COLOURS ARE READ THROUGH `bird._alt_mat` AND `bird._alt_mark`,
    which is the drawing code's own resolution rather than a copy of it. This
    file otherwise refuses to read internals; the reason it is allowed here is
    that the repaint column above has already proved the swap reaches the
    voxels, so what is left to check is which COLOURS it reached them with, and
    a second implementation of the sentinel logic is exactly the drift that
    would make the answer wrong.
    """
    rows = [(n, sm.load(SPECS / f"{n}.json")[0]) for n in names]
    rows = [(n, s) for n, s in rows if sm.get(s, "bird.sex_plumage") != "same"]
    print("\nTHE OTHER SEX'S COLOURS, AGAINST THE SAME CONTRAST FLOOR  "
          f"(floor {CONTRAST_FLOOR})\n")
    if not rows:
        print("  no species declares a sexed plumage.")
        return 0
    print(f"{'species':<26} {'sex':<8} {'wing/mark':>10} {'body/mark':>10} "
          f"{'head/mark':>10} {'bill/head':>10} {'back/belly':>11}  flags")
    bad = 0
    col, res = materials.color, materials.resolve
    for n, spec in rows:
        authored = str(sm.get(spec, "bird.sex_plumage"))
        other = "female" if authored == "male" else "male"
        for sex in (authored, other):
            s, _ = sm.patch(spec, {"bird.sex": sex})
            alt = _birdlib._alt(s)
            c = {k: col(_birdlib._alt_mat(s, k, alt)) for k in _ALT_MAT_SLOTS}
            mk = {k: _birdlib._alt_mark(s, k, alt) for k in _ALT_MARK_SLOTS}
            pairs = (
                ("wing", _contrast(c["wing"], c["mark"]),
                 mk["wing_mark"] != "none"),
                ("body", _contrast(c["belly"], c["mark"]),
                 mk["body_mark"] != "none"),
                ("head", _contrast(c["head"], c["head_mark"]),
                 mk["head_mark"] != "none"),
                ("bill", _contrast(c["bill"], c["head"]), True),
                # THE FIFTH PAIR, WHICH `readability` DOES NOT CHECK AT ALL and
                # which the hen mallard forced. Upperparts against underparts is
                # the division every field guide leads with, and on a bird whose
                # markings have all been swapped away it is the ONLY division
                # left -- a hen drawn brown over brown is a duck-shaped blob and
                # every marking check would pass her, because she has no
                # markings to check.
                #
                # CHECKED ONLY WHEN THERE IS NOTHING ELSE, and that condition is
                # the whole point rather than a let-off. Two species here are
                # deliberately one colour top to bottom -- a drake is grey above
                # and below, a winter ptarmigan is white -- and both measure
                # 1.00 here and are correct. What is not survivable is one
                # colour top to bottom AND no marking anywhere, which is a
                # silhouette with no information in it at all.
                ("back/belly", _contrast(c["back"], c["belly"]),
                 not any(mk[k] != "none" for k in _ALT_MARK_SLOTS)),
            )
            flags = [f"LOW CONTRAST on the {w} ({v:.2f})"
                     for w, v, on in pairs if on and v < CONTRAST_FLOOR]
            bad += bool(flags)

            def f(v, on):
                return "     -" if not on else f"{v:>6.2f}"

            print(f"{n if sex == authored else '':<26} {sex:<8} "
                  f"{f(pairs[0][1], pairs[0][2]):>10} "
                  f"{f(pairs[1][1], pairs[1][2]):>10} "
                  f"{f(pairs[2][1], pairs[2][2]):>10} "
                  f"{f(pairs[3][1], True):>10} {f(pairs[4][1], True):>11}  "
                  f"{'; '.join(flags)}")
    return bad


# The species every `alt` slot is exercised on. A songbird rather than a
# mallard, on purpose: this is a WIRING test and it has to run on a spec that
# authors none of the rows it is setting, so that what it measures is the
# mechanism and not the mallard.
_SLOT_RIG = "european-robin"


def _leak_floor(moved: float) -> float:
    """How far the CONTROL may drift before it counts as a leak.

    NOT A CONSTANT, AND THE FIRST VERSION WAS ONE. Setting an `alt` row changes
    the spec, `pipeline.rng_for` mixes the spec hash into the seed, and the
    marking phase therefore moves between the control build and the one it is
    compared against -- so a barred rig's marking share drifts by 1.3% with
    nothing wired wrong at all, and a flat 0.5% threshold reported the
    body-marking row as leaking on its first run. A real leak is the whole
    swap arriving on the wrong sex, so the bar is a QUARTER of the movement
    being tested, floored at 2% so that a tiny effect cannot be excused.
    """
    return max(2.0, 0.25 * abs(moved))


def _alt_slots_live(seeds: list[int]) -> int:
    """Does every one of the ten `alt` rows actually reach the voxels?

    THE TABLE ABOVE CANNOT ANSWER THIS AND IT IS THE OBVIOUS PLACE TO BE
    WRONG. Six of the seven colour slots and one of the three marking slots are
    used by exactly one species in the library, and two of the ten are used by
    none at all. A slot no species authors is a slot whose wiring has never
    been executed -- and this project's history is `bill_gape`, which was
    multiplied by `bill_depth` and therefore did nothing on any bird smaller
    than a heron, and shipped, and was found by a probe and not by an eye.

    So each row is set here on a species that authors none of them, to a colour
    or a marking the species wears nowhere, and the share of the bird wearing
    it is measured before and after. The reason this is a fair test rather than
    a rigged one is that it is the SAME rig for all ten: if the swap gate were
    wired to the wrong parameter, every row would read zero together.
    """
    print("\nEVERY ALT ROW, ON ONE RIG  (a slot no species authors is a slot "
          "that has never run)\n")
    spec, _ = sm.load(SPECS / f"{_SLOT_RIG}.json")
    # `sex_plumage=male` plus `sex=female` is the swap in force. The rig also
    # turns on all three markings, because a marking colour cannot be measured
    # on a bird that carries no marking -- which is the false alarm that makes
    # a no-op detector worse than none.
    spec, _ = sm.patch(spec, {
        "variation.amount": 0.0, "bird.sex_plumage": "male",
        "bird.head_mark": "cap", "bird.wing_mark": "bar",
        "bird.body_mark": "barred", "bird.mark_width": 0.5,
        # A LONG BILL ON PURPOSE. At the robin's authored 0.07 the bill and the
        # legs together are two voxels of a 600-voxel bird, and the alt bill row
        # measured 0.5% -- a real effect sitting exactly on the threshold, which
        # is indistinguishable from a dead one. The row being tested is the
        # wiring, not the robin.
        "bird.bill_frac": 0.22,
        "bird.length_m": 0.9, "bird.leg_len": 0.06})
    colour, = _spare_colours(spec, 1)
    base_m, _ = sm.patch(spec, {"bird.sex": "male"})
    base_f, _ = sm.patch(spec, {"bird.sex": "female"})
    print(f"  rig: {_SLOT_RIG} at 0.90 m, sex_plumage=male, all three markings "
          f"on, alt colour {colour}")
    print(f"\n{'alt row':<34} {'share before':>13} {'share after':>12} "
          f"{'moved':>7}  verdict")
    dead = 0

    def _mean_share(s, name) -> float:
        mat = materials.resolve(name)
        vals = []
        for sd in seeds:
            a = pipeline.build(s, sd)
            hist = a.stats["by_material"]
            vals.append(100.0 * hist.get(mat, 0) / max(sum(hist.values()), 1))
        return float(np.mean(vals))

    for slot in _ALT_MAT_SLOTS:
        path = f"materials.bird_alt_{slot}"
        before = _mean_share(base_f, colour)
        after = _mean_share(sm.patch(base_f, {path: colour})[0], colour)
        # AND THE CONTROL: the same row set, asked for as the sex the spec is
        # authored as. It must NOT move, or the gate is not a gate and every
        # bird in the library is wearing half of the other sex.
        leak = _mean_share(sm.patch(base_m, {path: colour})[0], colour)
        state = "moves" if after - before >= 0.5 else "DEAD"
        if abs(leak - before) >= _leak_floor(after - before):
            state = f"LEAKS TO THE AUTHORED SEX ({leak - before:.1f}%)"
        dead += state != "moves"
        print(f"{path:<34} {before:>12.1f}% {after:>11.1f}% "
              f"{after - before:>6.1f}%  {state}")

    # The markings are choices, so they are measured by how much of the bird
    # the marking colour covers when the region's mark is turned OFF for the
    # other sex. Off rather than on, because all three are already on in the
    # rig and a mark that cannot be turned off is a mark that is not being read.
    mark_colour = materials.resolve(sm.get(spec, "materials.bird_mark"))
    head_colour = materials.resolve(sm.get(spec, "materials.bird_head_mark"))
    for which, mat in (("head_mark", head_colour), ("wing_mark", mark_colour),
                       ("body_mark", mark_colour)):
        path = f"bird.sex_alt_{which}"

        def _share_of(s, m=mat) -> float:
            vals = []
            for sd in seeds:
                a = pipeline.build(s, sd)
                hist = a.stats["by_material"]
                vals.append(100.0 * hist.get(m, 0) / max(sum(hist.values()), 1))
            return float(np.mean(vals))

        before = _share_of(base_f)
        after = _share_of(sm.patch(base_f, {path: "none"})[0])
        leak = _share_of(sm.patch(base_m, {path: "none"})[0])
        state = "moves" if abs(after - before) >= 0.5 else "DEAD"
        if abs(leak - before) >= _leak_floor(after - before):
            state = f"LEAKS TO THE AUTHORED SEX ({leak - before:.1f}%)"
        dead += state != "moves"
        print(f"{path + ' -> none':<34} {before:>12.1f}% {after:>11.1f}% "
              f"{after - before:>6.1f}%  {state}")

    print(f"\n  {10 - dead}/10 alt rows reach the voxels and none of them leak "
          f"to the sex the spec is authored as.")
    return dead


# The species the pose A/B renders. Six rather than twenty, chosen to be
# unmistakable from each other at a glance and to span the shapes: the raven is
# the animal the pose bug was reported on, the eagle is the biggest thing in the
# library, the heron is all neck and legs, the kingfisher is the smallest bird
# that still carries a bill, the macaw is half tail and the robin is the
# reference songbird.
POSE_AB = ("common-raven", "golden-eagle", "grey-heron",
           "common-kingfisher", "scarlet-macaw", "european-robin")


def pose_ab(names: list[str], seed: int, out: Path) -> Path:
    """Folded and spread, same species and same seed, side by side.

    THIS IS THE RENDER THAT SETTLES IT, and the thing to look at is not the
    wings. It is that the two birds in a row are the same SIZE, carry the same
    markings in the same places and are the same colours -- one animal in two
    postures. Before `spec.seed_hash` they were two different individuals of
    one species and the row read as two birds.

    INDIVIDUAL VARIATION IS ON, and that is the check. The previous version of
    this sheet pinned `variation.amount` to 0 so that both halves would draw
    the species average and therefore match -- a workaround for exactly the
    defect this fixes, and one that hid it. With variation on, a row that
    matches can only match because both halves came off the same random stream.

    BOTH HALVES FROM THE ISOMETRIC, which is not what `render.camera_for` would
    choose. It sends a perched bird broadside and a flying one to the isometric,
    for good reasons -- but a comparison drawn through two different cameras is
    not a comparison. The isometric is the one that shows a spread wing without
    hiding a folded one.

    THE THIRD CELL IN EACH ROW IS THE CONTROL, and it is the reason this sheet
    can be believed. It is the same species spread at the NEXT seed. If the two
    left-hand cells match and the third one does not, the match means something;
    if all three match, individual variation is not reaching the animal and the
    sheet is proving nothing at all -- which is how a picture lies.

    ONE SCALE PER ROW, not one per page. The comparison being made is WITHIN a
    row, and a page-wide ruler that fits a golden eagle draws a kingfisher at
    eight pixels, which is a sheet nobody can read the markings off.
    """
    from forge import contact, render

    cells = []
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        row = []
        for tag, label, sd in (("perched", "folded", seed),
                               ("flying", "spread", seed),
                               ("flying", "spread", seed + 1)):
            s, _ = sm.patch(spec, {"bird.pose": tag})
            row.append((label, sd, pipeline.build(s, sd)))
        scale = render.scale_for_camera([a.grid.data.shape for _, _, a in row],
                                        "iso", 440)
        for label, sd, a in row:
            note = "  CONTROL: a different individual" if sd != seed else ""
            cells.append((render.view(a.grid, "iso", scale=scale),
                          f"{n}  {label}  seed {sd}  "
                          f"{a.stats['voxels']:,} vox{note}", []))

    img = contact.sheet(
        cells,
        title=f"pose A/B: one bird folded and spread, seed {seed}",
        subtitle=("variation ON, not pinned. Columns 1 and 2 must be the same "
                  "individual; column 3 must not be."),
        columns=3,
    )
    p = contact.save(img, out)
    print(f"  wrote {p}")
    return p


# The species the sex A/B renders, and this list is chosen by the TABLE rather
# than by taste: it is every species that authors a plumage swap, plus the two
# that move most on a ratio alone. Anything authored at 1.00/1.00 and `same` is
# left off, because three identical cells in a row is a picture that says
# nothing and trains the eye to skim the sheet.
SEX_AB = ("mallard-duck", "common-kestrel", "great-spotted-woodpecker",
          "rock-ptarmigan", "barn-swallow", "golden-eagle")


def sex_ab(names: list[str], seed: int, out: Path) -> Path:
    """Female, unsexed and male of one species, side by side.

    INDIVIDUAL VARIATION IS OFF, AND THAT IS THE OPPOSITE OF THE POSE SHEET.
    The pose sheet leaves it ON because what it has to prove is that two
    postures came off the same random stream, and pinning it would have made
    the two halves match for the wrong reason. Here the two halves are meant to
    be DIFFERENT BIRDS -- sex reseeds -- so leaving variation on would add a
    random size difference to every row and there would be no way to tell a
    real ratio from a seed that drew a big one. Each row is therefore one
    animal at its species average, drawn three times.

    THE MIDDLE CELL IS THE ONE TO READ ON A DIMORPHIC SPECIES. `unsexed` is the
    geometric mean on the ratios, so it should sit between its neighbours in
    size -- but on plumage it is whichever sex the spec is authored as, which
    means the middle mallard is a drake and matches the cell on one side of it
    exactly. That is the limitation of drawing colour rather than a number, and
    a sheet that hid it would be a sheet that lied.

    ONE SCALE PER ROW, not one per page, for the reason the pose sheet gives:
    a page-wide ruler that fits a golden eagle draws a swallow at eight pixels.

    PERCHED AND BROADSIDE ON EVERY ROW, INCLUDING THE THREE AUTHORED FLYING,
    and the first version of this sheet did neither. It drew each species in
    its authored pose through `render.camera_for`'s choice of camera, which
    sent the kestrel and the swallow to the isometric with their wings spread
    -- and a spread wing is the largest flat area on the animal, so the
    kestrel's rufous head against a slate one was four pixels beside a wing
    that fills the cell, and the swallow's two extra columns of tail were
    hidden behind its own swept primaries. Both differences ARE there; the
    sheet was drawing the part of the bird they are not on.

    Every difference on this sheet is a colour field on the flank or a tail,
    which is exactly what a perched broadside shows and what a spread-winged
    isometric hides. It is also the pose the measurement table uses, for the
    same reason and stated there. The pose is a posture and all twenty species
    are authorable in either, so this costs nothing.
    """
    from forge import contact, render

    cells = []
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        spec, _ = sm.patch(spec, {"variation.amount": 0.0,
                                  "bird.pose": "perched"})
        row = [(s, pipeline.build(sm.patch(spec, {"bird.sex": s})[0], seed))
               for s in ("female", "unsexed", "male")]
        scale = render.scale_for_camera([a.grid.data.shape for _, a in row],
                                        "side", 440)
        authored = str(sm.get(spec, "bird.sex_plumage"))
        ratios = (f"L{sm.get(spec, 'bird.sex_length'):g} "
                  f"T{sm.get(spec, 'bird.sex_tail'):g} {authored}")
        for s, a in row:
            note = f"  = the {authored}" if (authored != "same"
                                             and s == "unsexed") else ""
            cells.append((render.view(a.grid, "side", scale=scale),
                          f"{n}  {s}  [{ratios}]  "
                          f"{a.stats['voxels']:,} vox{note}", []))

    img = contact.sheet(
        cells,
        title=f"sex A/B: female, unsexed and male, seed {seed}",
        subtitle=("variation OFF, every row perched and broadside: one animal "
                  "drawn three times. [L T sex] = the two male:female ratios "
                  "and the sex the colours are authored as."),
        columns=3,
    )
    p = contact.save(img, out)
    print(f"  wrote {p}")
    return p


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--spec", default="european-robin",
                    help="which species the sweeps are run on "
                         "(default european-robin)")
    ap.add_argument("--seeds", type=int, default=4,
                    help="how many individuals to average each point over")
    ap.add_argument("--read", action="store_true", help="readability tests only")
    ap.add_argument("--lattice", action="store_true", help="lattice comparison only")
    ap.add_argument("--pose", action="store_true", help="pose comparison only")
    ap.add_argument("--sex", action="store_true",
                    help="male against female, in voxels and repainted area")
    ap.add_argument("--sex-ab", action="store_true",
                    help="render the female/unsexed/male sheet to out/birds/")
    ap.add_argument("--pose-ab", action="store_true",
                    help="render the folded/spread A/B sheet to out/birds/")
    ap.add_argument("--seed", type=int, default=7,
                    help="which individual the --pose-ab and --sex-ab sheets "
                         "draw (default 7)")
    args = ap.parse_args()

    seeds = list(range(1, args.seeds + 1))
    names = sorted(p.stem for p in SPECS.glob("*.json")
                   if sm.get(sm.load(p)[0], "kind") == "bird")
    if not names:
        print("no bird specs found; run tools/seed_birds.py", file=sys.stderr)
        return 2

    if args.pose_ab:
        missing = [n for n in POSE_AB if n not in names]
        if missing:
            # A renamed species must not turn this render into a shorter sheet
            # that still looks finished.
            print(f"POSE_AB names specs that do not exist: {', '.join(missing)}",
                  file=sys.stderr)
            return 2
        pose_ab(list(POSE_AB), args.seed, ROOT / "out" / "birds" / "birds-pose-ab.png")
        return 0
    if args.sex_ab:
        missing = [n for n in SEX_AB if n not in names]
        if missing:
            print(f"SEX_AB names specs that do not exist: {', '.join(missing)}",
                  file=sys.stderr)
            return 2
        sex_ab(list(SEX_AB), args.seed, ROOT / "out" / "birds" / "birds-sex-ab.png")
        return 0
    if args.lattice:
        lattice(names, seeds)
        return 0
    if args.pose:
        return 1 if pose(names, seeds) else 0
    if args.sex:
        return 1 if sexes(names, seeds) else 0
    if args.read:
        return 1 if readability(names, seeds) else 0

    base, _ = sm.load(SPECS / f"{args.spec}.json")
    dead = sweep(base, seeds)
    variation(base, seeds)
    bad = readability(names, seeds)
    bad += sexes(names, seeds)
    print(f"\n{dead} parameter(s) measured as DEAD; "
          f"{bad} species carry a readability or sex flag.")
    print("A DEAD parameter is not a tuning problem. It is a wiring problem.")
    return 1 if dead else 0


if __name__ == "__main__":
    raise SystemExit(main())
