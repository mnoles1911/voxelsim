"""Does each fish slider actually do anything? Measure it and say so.

THE SIGNATURE FAILURE OF THIS PROJECT IS THE SILENT NO-OP. A weathering pass
removed 20 voxels out of 90,000 for months while reporting success. A `--fit`
flag produced byte-identical images. A slider's value was consumed twenty-five
lines before the code that modified it. Every one of those ran, cost time, and
changed nothing, and every one was found by a person looking at a picture.

So every mechanism in `forge/fish.py` gets a number here that moves when its
slider moves, and this tool prints the number and says DEAD when it does not.

    python tools/fishprobe.py                 # sweep every fish parameter
    python tools/fishprobe.py --seeds 6       # more seeds, tighter answer
    python tools/fishprobe.py --read          # the three readability tests
    python tools/fishprobe.py --lattice       # 1 cm against 2 cm and 5 cm
    python tools/fishprobe.py --marks         # shaped colour boundaries, in voxels
    python tools/fishprobe.py --head          # head span: the cephalofoil
    python tools/fishprobe.py --sex           # male against female, in voxels

WHY IT AVERAGES OVER SEEDS. Changing any parameter changes the spec hash, and
`pipeline.rng_for` mixes the hash into the seed -- so a one-seed A/B is not the
same fish twice, it is two different fish. Four seeds is enough to see a real
effect through that; one is enough to invent one.

WHAT "MOVES" MEANS HERE. A parameter passes if its measurement changes by more
than one voxel AND by more than 4% across its authored range. Both, because
either alone lies: a percentage is meaningless on a quantity that is 2 voxels,
and one voxel of change on a 40-voxel measurement is rounding.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

import _path  # noqa: F401  (sys.path bootstrap)
from forge import materials, palette, pipeline, render, spec as sm

ROOT = Path(__file__).resolve().parents[1]
SPECS = ROOT / "specs"


# --- measurements -----------------------------------------------------------
#
# Each of these is a number a HUMAN could check off a render, which is the point:
# a probe that measures an internal variable proves the variable exists, not that
# it reached the voxels.


def _occ(asset):
    return asset.grid.data != 0


def m_length(a) -> float:
    """Nose to the end of the tail fin, in voxels."""
    return float(_occ(a).shape[0])


def m_depth(a) -> float:
    """Deepest point of the whole animal, in voxels."""
    sil = _occ(a).any(axis=1)
    return float(sil.sum(axis=1).max())


def m_body_depth(a) -> float:
    """Deepest BODY column, in voxels, ignoring the fins.

    Fins are excluded by taking the median column depth over the middle half of
    the fish rather than the maximum -- a dorsal fin is tall and narrow, so it
    barely moves the median and dominates the max. Without this, every "does
    the body get deeper" question was answered by the dorsal fin.
    """
    sil = _occ(a).any(axis=1)
    n = sil.shape[0]
    cols = sil[int(0.3 * n):int(0.7 * n)].sum(axis=1)
    return float(np.median(cols)) if cols.size else 0.0


def m_width(a) -> float:
    return float(_occ(a).any(axis=2).sum(axis=1).max())


def m_wrist_width(a) -> float:
    """Width across the fish at the caudal peduncle, in voxels.

    `fish.width_follow` decides how hard the fish flattens TOWARD ITS TAIL, and
    the widest point of a fish is at its deepest station where the profile is 1
    by construction -- so the maximum width cannot see this parameter at all
    and reported it as dead across its whole range. The wrist is where it acts.

    The window starts a fifth of the way along rather than at the tail, because
    the tail FIN is one voxel thick by definition -- a minimum taken over the
    aft 45% returned 1 whatever the body was doing, which is the same class of
    mistake as measuring a dorsal fin by voxel count.
    """
    plan = _occ(a).any(axis=2)
    n = plan.shape[0]
    w = plan.sum(axis=1)[int(0.20 * n):int(0.50 * n)]
    w = w[w > 0]
    return float(w.min()) if w.size else 0.0


def m_notch_run(a) -> float:
    """How many columns of the tail fin carry the notch.

    `fish.caudal_fork` is the share of the fin's LENGTH the V eats into, so the
    thing it controls is how far forward the notch reaches -- not how deep the
    gap is at the trailing edge, which is held at a minimum lobe width and is
    therefore the same at both ends of the range. Measuring the wrong one is
    how the inverted-slider bug stayed invisible.
    """
    sil = _occ(a).any(axis=1)
    n = sil.shape[0]
    runs = 0
    for x in range(max(1, int(0.25 * n))):
        on = np.flatnonzero(sil[x])
        if on.size >= 2 and (on[-1] - on[0] + 1) - on.size > 0:
            runs += 1
    return float(runs)


def m_wrist_section(a) -> float:
    """How full the cross-section is AT THE WRIST, as a percentage of its box.

    `fish.section_tail` acts only at the tail, so the mid-body section fill
    that `fish.section` is measured by cannot see it at all.
    """
    occ = _occ(a)
    n = occ.shape[0]
    x = int(0.28 * n)                     # aft of the fins, forward of the tail
    sect = occ[x]
    if not sect.any():
        return 0.0
    ys, zs = np.nonzero(sect)
    box = (ys.max() - ys.min() + 1) * (zs.max() - zs.min() + 1)
    return 100.0 * sect.sum() / max(box, 1)


def m_lobe_bias(a) -> float:
    """How much further aft the UPPER half of the tail reaches than the lower.

    A shark's heterocercy, measured the way it is defined -- as a difference in
    LENGTH between the two lobes, not in height. Positive is upper-longer.
    """
    sil = _occ(a).any(axis=1)
    nx, nz = sil.shape
    # SPLIT ABOUT THE BODY AXIS AT THE PEDUNCLE, not about the middle of the
    # whole silhouette. The silhouette's midpoint is dragged upward by the
    # dorsal fin, which on a shark is the tallest thing on the animal -- so
    # "the upper half" became "the dorsal fin", the measurement was constant,
    # and it called a working slider dead.
    col = sil[int(0.25 * nx)]
    zs = np.flatnonzero(col if col.any() else sil.any(axis=0))
    if zs.size == 0:
        return 0.0
    mid = 0.5 * (zs[0] + zs[-1])
    up = sil[:, int(mid) + 1:]
    dn = sil[:, :int(mid)]
    fx_up = np.flatnonzero(up.any(axis=1))
    fx_dn = np.flatnonzero(dn.any(axis=1))
    if fx_up.size == 0 or fx_dn.size == 0:
        return 0.0
    # x = 0 is the trailing edge, so the SMALLER first index reaches further.
    return float(fx_dn[0] - fx_up[0])


def m_dorsal_at(a) -> float:
    """Where the tallest thing on the back is, as a fraction of the length."""
    sil = _occ(a).any(axis=1)
    n = sil.shape[0]
    if n < 2:
        return 0.0
    tops = np.array([np.flatnonzero(c)[-1] if c.any() else 0 for c in sil])
    return float((n - 1 - int(np.argmax(tops))) / (n - 1))


def m_pectoral_run(a) -> float:
    """How many columns the paired fins occupy fore-and-aft.

    `fish.pectoral_aspect` changes a flipper's CHORD, not how far it sticks
    out, so the reach measurement that `fish.pectoral` uses cannot see it.
    Measured as the columns wider than the body's own median width.
    """
    plan = _occ(a).any(axis=2)
    w = plan.sum(axis=1)
    live = w[w > 0]
    if live.size == 0:
        return 0.0
    return float((w > np.median(live) + 1).sum())


def m_patch(a) -> float:
    mat = materials.resolve(sm.get(a.spec, "materials.fish_patch"))
    return float(a.stats["by_material"].get(mat, 0))


def m_deepest_at(a) -> float:
    """Where the fish is deepest, as a fraction of its length from the nose."""
    sil = _occ(a).any(axis=1)
    n = sil.shape[0]
    if n < 2:
        return 0.0
    return float((n - 1 - int(np.argmax(sil.sum(axis=1)))) / (n - 1))


def m_wrist(a) -> float:
    """Depth of the thinnest column between the tail fin and the body.

    Measured as the minimum column depth in the aft 40%, which is what a caudal
    peduncle IS. Bounded below by 1 because the axis run is always there.
    """
    sil = _occ(a).any(axis=1)
    n = sil.shape[0]
    cols = sil[:int(0.4 * n)].sum(axis=1)
    cols = cols[cols > 0]
    return float(cols.min()) if cols.size else 0.0


def m_tail_span(a) -> float:
    """How tall the tail fin is, in voxels: the aft 12% of the silhouette."""
    sil = _occ(a).any(axis=1)
    n = sil.shape[0]
    return float(sil[:max(1, int(0.12 * n))].sum(axis=1).max())


def m_tail_notch(a) -> float:
    """Depth of the fork, in voxels.

    The largest run of EMPTY cells enclosed within the tail fin's silhouette,
    measured column by column from the trailing edge. A fork is a hole in the
    middle of the fin; a truncate tail has none. This is the same idea as
    `rock.daylight` and it exists for the same reason -- "the carve reported
    success" is not the same fact as "there is a hole in it".
    """
    # MEASURED IN THE PLANE THE TAIL ACTUALLY LIES IN. A whale's fluke is
    # horizontal, so its notch is a gap across Y and a Z silhouette cannot see
    # it -- every cetacean reported a fork of 0, which is a column of zeroes
    # pretending to mean something.
    horizontal = sm.get(a.spec, "fish.caudal_plane") == "horizontal"
    sil = _occ(a).any(axis=2 if horizontal else 1)
    n = sil.shape[0]
    best = 0
    for x in range(max(1, int(0.10 * n))):
        col = sil[x]
        on = np.flatnonzero(col)
        if on.size < 2:
            continue
        gap = int((on[-1] - on[0] + 1) - on.size)
        best = max(best, gap)
    return float(best)


def m_dorsal(a) -> float:
    """How far the tallest thing on the back stands above the body line."""
    sil = _occ(a).any(axis=1)
    n = sil.shape[0]
    mid = sil[int(0.25 * n):int(0.85 * n)]
    if not mid.size:
        return 0.0
    tops = np.array([np.flatnonzero(c)[-1] if c.any() else 0 for c in mid])
    return float(tops.max() - np.median(tops))


def m_belly_fin(a) -> float:
    """The same, downward: how far the anal and pelvic fins hang."""
    sil = _occ(a).any(axis=1)
    n = sil.shape[0]
    mid = sil[int(0.15 * n):int(0.85 * n)]
    if not mid.size:
        return 0.0
    bots = np.array([np.flatnonzero(c)[0] if c.any() else 0 for c in mid])
    return float(np.median(bots) - bots.min())


def m_pectoral(a) -> float:
    """Widest point minus the body's own width at the same station.

    A pectoral fin is the only thing on a fish that sticks out sideways, so
    "how much wider than the body is the widest slice" measures it and nothing
    else.
    """
    plan = _occ(a).any(axis=2)
    w = plan.sum(axis=1)
    if not w.size:
        return 0.0
    return float(w.max() - np.median(w[w > 0]))


def m_pattern(a) -> float:
    """Share of the fish wearing the marking material, as a percentage.

    Reads the material histogram rather than the geometry, because a marking IS
    an assignment of materials. Zero here with a pattern selected is the exact
    shape of the silent no-op this file exists to catch.
    """
    spec = a.spec
    # A species with NO marking has no marking coverage, whatever colour the
    # unused marking slot happens to hold. Without this the readability table
    # reported a plain grey dolphin as 51% marked, because its unused pattern
    # material and its back are both `skin_dark`.
    #
    # AN HOURGLASS IS A MARKING, and it is the fourth time this measurement has
    # been wrong. `fish.pattern` is `none` on a common dolphin, but the forward
    # half of its pinched flank is painted in the marking material -- that IS
    # its one marking -- so the readability table reported 0% ink on the one
    # species whose whole identity is a colour boundary, and the value-contrast
    # gate never ran on it. It should have: the first buff chosen for that
    # patch measured a contrast of 1.05 against the flank beside it.
    marked = (sm.get(spec, "fish.pattern") != "none"
              or (sm.get(spec, "fish.field_curve") == "hourglass"
                  and float(sm.get(spec, "fish.curve_amount")) > 0.0))
    if not marked:
        return 0.0
    mat = materials.resolve(sm.get(spec, "materials.fish_pattern"))
    hist = a.stats["by_material"]
    total = max(sum(hist.values()), 1)
    # Only counts if the marking material is not also the flank material -- a
    # pattern painted in the body colour covers voxels and shows nothing.
    flank = materials.resolve(sm.get(spec, "materials.fish_flank"))
    return 0.0 if mat == flank else 100.0 * hist.get(mat, 0) / total


def m_back(a) -> float:
    mat = materials.resolve(sm.get(a.spec, "materials.fish_back"))
    hist = a.stats["by_material"]
    return 100.0 * hist.get(mat, 0) / max(sum(hist.values()), 1)


def m_belly(a) -> float:
    mat = materials.resolve(sm.get(a.spec, "materials.fish_belly"))
    hist = a.stats["by_material"]
    return 100.0 * hist.get(mat, 0) / max(sum(hist.values()), 1)


def m_eye(a) -> float:
    mat = materials.resolve(sm.get(a.spec, "materials.fish_eye"))
    hist = a.stats["by_material"]
    return float(hist.get(mat, 0))


def m_voxels(a) -> float:
    return float(a.stats["voxels"])


def m_section_fill(a) -> float:
    """How full the deepest cross-section is, as a percentage of its box.

    This is the ONLY measurement that can see the superellipse exponent. A
    rounder section fills more of its bounding rectangle; a diamond fills half.
    It is here because the research says this parameter is nearly worthless at
    this resolution and that claim deserves a number rather than a shrug.
    """
    occ = _occ(a)
    sil = occ.any(axis=1)
    x = int(np.argmax(sil.sum(axis=1)))
    sect = occ[x]
    if not sect.any():
        return 0.0
    ys, zs = np.nonzero(sect)
    box = (ys.max() - ys.min() + 1) * (zs.max() - zs.min() + 1)
    return 100.0 * sect.sum() / max(box, 1)


def m_eye_at(a) -> float:
    """Where the eye is, as a fraction of the length back from the nose.

    `fish.head_frac` moves the eye and the pectoral fins and changes nothing
    else, so counting eye VOXELS to test it measured the wrong thing and came
    back weak. Position is what the parameter controls, so position is what
    gets measured.
    """
    occ = _occ(a)
    mat = materials.resolve(sm.get(a.spec, "materials.fish_eye"))
    xs = np.nonzero((a.grid.data == mat).any(axis=(1, 2)))[0]
    n = occ.shape[0]
    if xs.size == 0 or n < 2:
        return 0.0
    return float((n - 1 - xs.max()) / (n - 1))


def m_dorsal_run(a) -> float:
    """How many columns carry something standing above the body line.

    `fish.dorsal_len` makes the fin LONGER, not taller and not much heavier --
    a triangular fin of twice the length and the same height is about the same
    area -- so measuring it by voxel count came back at 3% and called a working
    slider dead. Length is what it controls.
    """
    sil = _occ(a).any(axis=1)
    n = sil.shape[0]
    mid = sil[int(0.2 * n):int(0.9 * n)]
    if not mid.size:
        return 0.0
    tops = np.array([np.flatnonzero(c)[-1] if c.any() else 0 for c in mid])
    # TWO VOXELS ABOVE THE BODY LINE, not merely above the median. A fish's
    # back is a curve, so about half its columns sit above their own median
    # whether or not there is a fin on them -- which made this return the same
    # number for every dorsal shape INCLUDING `none`, and call a working choice
    # dead. Two voxels is also the pixel-art floor for a feature that reads.
    return float((tops >= np.median(tops) + 2).sum())


def m_belly_run(a) -> float:
    """Columns carrying something two voxels or more BELOW the body line.

    The downward mirror of `m_dorsal_run`, and it exists for the same reason:
    `fish.anal_len` makes the fin longer, not heavier, so a voxel count could
    not see it. On a great white, whose anal fin is a tenth of its body depth,
    the voxel count moved 1% across the slider's whole range and called a
    working fin dead.
    """
    sil = _occ(a).any(axis=1)
    n = sil.shape[0]
    mid = sil[int(0.15 * n):int(0.85 * n)]
    if not mid.size:
        return 0.0
    bots = np.array([np.flatnonzero(c)[0] if c.any() else 0 for c in mid])
    return float((bots <= np.median(bots) - 2).sum())


def m_mark_runs(a) -> float:
    """How many separate bands of marking there are along the fish.

    `fish.pattern_count` sets the NUMBER of bars while `fish.pattern_width`
    sets how much of each period is bar, so at a fixed width more bars cover
    the same total area -- which is correct, and which made a coverage
    measurement report the count slider as doing nothing.
    """
    mat = materials.resolve(sm.get(a.spec, "materials.fish_pattern"))
    col = (a.grid.data == mat).any(axis=(1, 2)).astype(np.int8)
    return float(np.count_nonzero(np.diff(np.concatenate(([0], col))) > 0))


def m_head_span(a) -> float:
    """Widest point of the FRONT QUARTER of the animal, in voxels.

    The cephalofoil, and it is deliberately measured over a window rather than
    at a station: the hammer's widest point sits at the back of the hammer, and
    where that lands depends on the span, so a fixed station would measure the
    leading edge on a wide head and the neck on a narrow one.

    The front quarter and not the front tenth, because the pectoral fins are
    the only other thing on a fish that sticks out sideways and they insert at
    0.28 of the body -- outside this window on every species here.
    """
    plan = _occ(a).any(axis=2)
    n = plan.shape[0]
    return float(plan[int(0.75 * n):].sum(axis=1).max()) if n else 0.0


def m_head_pct(a) -> float:
    """The same, as a percentage of the animal's total length.

    This is the number the published figure is quoted in -- a scalloped
    hammerhead's cephalofoil is 25-32% of TOTAL length, tail included -- so
    this column can be read straight against the source.
    """
    n = _occ(a).shape[0]
    return 100.0 * m_head_span(a) / max(n, 1)


def _column_line(a, role: str, upward: bool):
    """Height of one colour field's edge, per column, in voxels off the bottom.

    `upward` reads the TOP of the field, which is what a belly has; False reads
    the BOTTOM, which is what a back has. Columns where the field is absent
    come back as NaN.

    MEASURED OFF THE COLUMN'S OWN BOTTOM AND COMPARED AGAINST A CONTROL, never
    read as an absolute. A level boundary is a constant FRACTION of the body's
    depth, and the body's depth halves between the shoulder and the tail wrist,
    so a level boundary already falls by two or three voxels along a dolphin.
    Calling that "the boundary moved" would report the flat case as a working
    curve, which is the same class of mistake as counting a dorsal fin by
    voxels. `_curve_against_flat` takes the difference against the same animal
    with the curve switched off, which is what `tools/birdprobe.py` had to do
    for ink coverage and for the same reason.
    """
    mat = materials.resolve(sm.get(a.spec, f"materials.fish_{role}"))
    data = a.grid.data
    sil = (data != 0).any(axis=1)
    hit = (data == mat).any(axis=1)
    nx = sil.shape[0]
    out = np.full(nx, np.nan)
    for x in range(nx):
        col = np.flatnonzero(sil[x])
        if col.size < 3:
            continue
        marked = np.flatnonzero(hit[x])
        if marked.size == 0:
            continue
        z = int(marked[-1] if upward else marked[0])
        out[x] = z - int(col[0])
    return out


def m_curve_rise(a) -> float:
    """How far the pale edge climbed, in voxels, against the same fish drawn flat."""
    return _curve_against_flat(a.spec, a.seed)[1]


def m_curve_dip(a) -> float:
    """How far the dark edge reached down, in voxels, against the flat control."""
    return _curve_against_flat(a.spec, a.seed)[0]


def m_curve_where(a) -> float:
    """Where the pale edge climbed MOST, as a fraction back from the nose.

    `fish.curve_at` moves the flame without changing how much of the fish is
    pale, so a coverage measurement cannot see it at all -- the same trap
    `fish.pattern_pos` fell into. Position is what the parameter controls, so
    position is what gets measured.

    MEASURED AS A DIFFERENCE AND NOT AS AN ABSOLUTE, which is the second time
    that distinction has mattered here. The first version took the highest
    point of the pale edge outright, and on a brown trout it answered 0.76 at
    both ends of the slider: a level boundary is a constant fraction of a depth
    that varies threefold along the animal, so the pale edge is already highest
    at the deepest station and a two-voxel flame never moves the maximum. The
    control makes the question "where did it MOVE", which is the question.
    """
    dif = _curve_against_flat(a.spec, a.seed)[4]
    n = dif.size
    if n < 2 or not np.isfinite(dif).any() or np.nanmax(dif) <= 0.0:
        return 0.0
    return float((n - 1 - int(np.nanargmax(dif))) / (n - 1))


def m_mark_height(a) -> float:
    """Mean height of the marking within the fish, 0 belly to 1 back.

    Moving a stripe up and down does not change how much of the fish it
    covers, so coverage could not see `fish.pattern_pos` at all.
    """
    mat = materials.resolve(sm.get(a.spec, "materials.fish_pattern"))
    xs, ys, zs = np.nonzero(a.grid.data == mat)
    if zs.size == 0:
        return 0.0
    nz = a.grid.data.shape[2]
    return float(zs.mean() / max(nz - 1, 1))


# parameter -> (measurement, unit, label). Swept low against high.
SWEEPS = [
    ("fish.length_m", m_length, "vox", "length, voxels"),
    ("fish.depth_ratio", m_body_depth, "vox", "body depth, voxels"),
    ("fish.width_ratio", m_width, "vox", "width, voxels"),
    ("fish.depth_at", m_deepest_at, "frac", "deepest at, frac of length"),
    ("fish.fullness", m_voxels, "vox", "solid voxels"),
    ("fish.snout", m_voxels, "vox", "solid voxels"),
    ("fish.peduncle", m_wrist, "vox", "wrist depth, voxels"),
    ("fish.belly", m_belly_fin, "vox", "belly fin reach, voxels"),
    ("fish.width_follow", m_wrist_width, "vox", "wrist width, voxels"),
    ("fish.section", m_section_fill, "pct", "section fill, % of box"),
    ("fish.section_tail", m_wrist_section, "pct", "wrist section fill, % of box"),
    ("fish.head_frac", m_eye_at, "frac", "eye at, frac of length"),
    ("fish.head_width", m_head_span, "vox", "head span, voxels"),
    ("fish.caudal_len", m_length, "vox", "length, voxels"),
    ("fish.caudal_span", m_tail_span, "vox", "tail span, voxels"),
    ("fish.caudal_fork", m_notch_run, "vox", "notched columns"),
    ("fish.caudal_upper", m_lobe_bias, "vox", "upper lobe reach minus lower"),
    ("fish.dorsal_start", m_dorsal, "vox", "back fin height, voxels"),
    ("fish.dorsal_len", m_dorsal_run, "vox", "back fin length, columns"),
    ("fish.dorsal2_height", m_dorsal, "vox", "back fin height, voxels"),
    ("fish.dorsal2_len", m_dorsal_run, "vox", "back fin length, columns"),
    ("fish.dorsal2_start", m_dorsal_at, "frac", "back fin at, frac of length"),
    ("fish.dorsal_height", m_dorsal, "vox", "back fin height, voxels"),
    ("fish.anal_height", m_belly_fin, "vox", "belly fin reach, voxels"),
    ("fish.anal_len", m_belly_run, "vox", "belly fin length, columns"),
    ("fish.pectoral", m_pectoral, "vox", "widest minus body, voxels"),
    ("fish.pectoral_aspect", m_pectoral_run, "vox", "flipper chord, columns"),
    ("fish.pelvic", m_belly_fin, "vox", "belly fin reach, voxels"),
    # Measured by LENGTH, not by voxel count: four threads off a snout are
    # a couple of dozen voxels on a fish of five hundred, so the total
    # could not see them and reported a working slider as weak. What they
    # do change is how far forward the animal reaches.
    ("fish.barbels", m_length, "vox", "length, voxels"),
    ("fish.barbel_len", m_length, "vox", "length, voxels"),
    ("fish.fin_thick", m_voxels, "vox", "solid voxels"),
    ("fish.eye", m_eye, "vox", "eye voxels"),
    ("fish.eye_patch", m_patch, "vox", "eye-patch voxels"),
    ("fish.blowhole", m_eye, "vox", "blowhole voxels"),
    ("fish.fin_min_vox", m_dorsal, "vox", "back fin height, voxels"),
    ("fish.back_frac", m_back, "pct", "back colour, % of fish"),
    ("fish.belly_frac", m_belly, "pct", "belly colour, % of fish"),
    # The shaped boundaries. `curve_amount` is measured by how much of the fish
    # each field covers, because lifting the pale edge up the flank IS more
    # pale fish; `curve_at` is measured by POSITION, because moving the flame
    # along the animal does not change how much of it is pale.
    ("fish.curve_amount", m_belly, "pct", "belly colour, % of fish"),
    ("fish.curve_at", m_curve_where, "frac", "pale edge peaks at, frac of length"),
    # Sexual dimorphism. Each ratio is measured by the thing it scales, and
    # each needs a SEX selected before it can do anything -- see SETUP_FOR.
    ("fish.sex_length", m_length, "vox", "length, voxels"),
    ("fish.sex_dorsal", m_dorsal, "vox", "back fin height, voxels"),
    ("fish.sex_pectoral", m_pectoral, "vox", "widest minus body, voxels"),
    ("fish.pattern_count", m_mark_runs, "vox", "bands of marking"),
    ("fish.pattern_width", m_pattern, "pct", "marking, % of fish"),
    ("fish.pattern_pos", m_mark_height, "frac", "marking height, 0 belly 1 back"),
    ("fish.pattern_scale", m_pattern, "pct", "marking, % of fish"),
    ("fish.pattern_strength", m_pattern, "pct", "marking, % of fish"),
]

# What a parameter needs SWITCHED ON before it can possibly do anything.
#
# Sweeping `barbel_len` on a species with no barbels measures nothing and
# reports DEAD -- which is a false alarm, and a false alarm on a silent-no-op
# detector is worse than no detector at all, because it teaches you to skim the
# output. The first run of this tool did exactly that; this table is the fix.
# Same for the five pattern parameters, each of which only means something
# under a particular marking.
# EVERY MEASUREMENT THAT READS A MATERIAL NEEDS THAT MATERIAL TO BE UNIQUE.
#
# The probe counts eye voxels by counting voxels of the eye MATERIAL, and
# marking coverage the same way. That is exactly right on a species that uses a
# different colour for each role, and silently wrong on one that does not --
# and plenty do not: a great white's back, its eye and its markings are all
# `skin_dark`, so "count the eye" counted the entire back and the eye slider
# measured as doing nothing across its whole range.
#
# So a material sweep repaints the animal first: everything silver except the
# one thing being measured, which goes dark. It changes no geometry, and it is
# the difference between measuring a feature and measuring a colour.
_ALL_MATS = ("back", "flank", "belly", "fin", "pattern", "eye", "patch")


def _isolate(name: str) -> dict:
    """Paint every material silver except `name`, which goes dark."""
    out = {f"materials.fish_{m}": "skin_silver" for m in _ALL_MATS}
    out[f"materials.fish_{name}"] = "skin_dark"
    return out


SETUP_FOR = {
    "fish.pattern_count": {"fish.pattern": "bars", **_isolate("pattern")},
    "fish.pattern_width": {"fish.pattern": "bars", **_isolate("pattern")},
    "fish.pattern_pos": {"fish.pattern": "stripe", **_isolate("pattern")},
    "fish.pattern_scale": {"fish.pattern": "spots", **_isolate("pattern")},
    "fish.pattern_strength": {"fish.pattern": "mottle", **_isolate("pattern")},
    "fish.barbel_len": {"fish.barbels": 4},
    "fish.barbels": {"fish.barbel_len": 0.25},
    # The eye is drawn in the marking colour on most species and in the BACK
    # colour on several, so counting eye voxels counted those too.
    "fish.eye": {"fish.pattern": "none", "fish.eye_patch": 0, **_isolate("eye")},
    "fish.head_frac": {"fish.pattern": "none", "fish.eye_patch": 0,
                       **_isolate("eye")},
    # The new mechanisms. Each needs the thing it acts on switched on first.
    "fish.eye_patch": {"fish.pattern": "none", "fish.eye": 0,
                       **_isolate("patch")},
    "fish.blowhole": {"fish.pattern": "none", "fish.eye": 0,
                      "fish.eye_patch": 0, **_isolate("eye")},
    "fish.dorsal2_height": {"fish.dorsal_height": 0.0},
    "fish.dorsal2_len": {"fish.dorsal_height": 0.0, "fish.dorsal2_height": 0.5},
    "fish.dorsal2_start": {"fish.dorsal_height": 0.0, "fish.dorsal2_height": 0.5},
    "fish.caudal_upper": {"fish.caudal_shape": "pointed"},
    # A FLOOR NEEDS SOMETHING TO FLOOR. Setting the fin heights to zero turns
    # the fins OFF -- the generator skips a fin with no height -- so the floor
    # had nothing to act on and measured as dead. 0.01 of body depth is a fin
    # the species asked for and cannot have, which is exactly the case this
    # parameter exists for, and it is what a blue whale's dorsal fin is.
    "fish.fin_min_vox": {"fish.dorsal_height": 0.01, "fish.anal_height": 0.0,
                         "fish.dorsal2_height": 0.0},
    # A CURVE NEEDS A SHAPE SELECTED AND A MARKING OUT OF THE WAY. `flat` is
    # the default, so sweeping the amount or the station on an unmodified spec
    # measures a boundary that was never asked to bend -- the exact false
    # DEAD the `fin_min_vox` entry above exists for. And the trout's spots are
    # painted over the belly line, so the pale edge the measurement reads is
    # partly a spot; the marking goes off.
    "fish.curve_amount": {"fish.field_curve": "flame", "fish.pattern": "none",
                          "fish.curve_at": 0.60, **_isolate("belly")},
    "fish.curve_at": {"fish.field_curve": "flame", "fish.curve_amount": 0.45,
                      "fish.pattern": "none", **_isolate("belly")},
    # A RATIO WITH NO SEX CHOSEN IS 1.0 BY DESIGN. `unsexed` is the default and
    # it deliberately ignores all three ratios, so sweeping one on an
    # unmodified spec is guaranteed to measure nothing and report a working
    # parameter DEAD. Swept as the MALE, which is the sex the ratio multiplies
    # up.
    "fish.sex_length": {"fish.sex": "male"},
    "fish.sex_dorsal": {"fish.sex": "male"},
    "fish.sex_pectoral": {"fish.sex": "male"},
}

# Shape choices are not numbers, so they are swept as a set rather than a range.
CHOICE_SWEEPS = [
    ("fish.caudal_shape", ("forked", "truncate", "rounded", "pointed", "none"),
     m_tail_notch, "vox", "fork depth, voxels"),
    ("fish.caudal_shape", ("forked", "truncate", "rounded", "pointed", "none"),
     m_tail_span, "vox", "tail span, voxels"),
    ("fish.dorsal_shape", ("triangular", "sail", "spiny", "ridge", "none"),
     m_dorsal, "vox", "back fin height, voxels"),
    ("fish.dorsal_shape", ("triangular", "sail", "spiny", "ridge", "none"),
     m_dorsal_run, "vox", "back fin length, columns"),
    ("fish.pattern", ("none", "stripe", "bars", "spots", "mottle", "saddle"),
     m_pattern, "pct", "marking, % of fish"),
    # The four boundary shapes, measured on the two fields they bend. `cape`
    # must move the BACK and leave the belly alone; `flame` the other way
    # round; `hourglass` both. Two rows, because one of them would pass a
    # version that bent the wrong edge.
    ("fish.field_curve", ("flat", "cape", "flame", "hourglass"),
     m_back, "pct", "back colour, % of fish"),
    ("fish.field_curve", ("flat", "cape", "flame", "hourglass"),
     m_belly, "pct", "belly colour, % of fish"),
    ("fish.sex", ("unsexed", "female", "male"),
     m_dorsal, "vox", "back fin height, voxels"),
]

# What the choice sweeps need set up first, same idea as SETUP_FOR.
CHOICE_SETUP = {
    # A CURVE NEEDS SOMEWHERE TO BEND INTO. The two countershading fractions
    # decide how much flank there is between the dark and the pale, and on a
    # great white they are 0.56 and 0.40 -- four hundredths of a body apart.
    # Swept on that species as authored, the cape had nowhere to dip to and
    # measured as doing nothing, which is true of the great white and false of
    # the mechanism. Widened to the gap a dolphin has.
    "fish.field_curve": {"fish.curve_amount": 0.45, "fish.curve_at": 0.60,
                         "fish.pattern": "none", "fish.back_frac": 0.40,
                         "fish.belly_frac": 0.20},
    # Without ratios there is nothing for the sexes to differ BY, and the
    # choice would measure as dead on a species that has no dimorphism -- which
    # is true of that species and false of the mechanism.
    "fish.sex": {"fish.sex_dorsal": 1.70, "fish.sex_length": 1.30,
                 "fish.sex_pectoral": 1.70},
}


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
# units are a FRACTION OF THE FISH would demand a change of 1.0, which no
# fraction can make, and the first run of this tool duly reported two working
# sliders as weak on exactly that basis.
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
    # Not to flatter the numbers -- to make them mean anything. Every fish spec
    # authors `variation.height` at 0.18, so two seeds of one species differ in
    # length by about three voxels of standard deviation. Four barbels add three
    # voxels. Averaged over four seeds the barbels sat inside that noise and this
    # tool called a slider that plainly works DEAD -- which is the false alarm
    # that makes a no-op detector worse than useless, because it teaches you to
    # skim its output.
    #
    # Seeds are still averaged over, because with the size pinned the seed still
    # moves the marking phase and the blotch noise, and those are exactly the
    # parameters a single seed would lie about. That variation ITSELF works is a
    # separate test, immediately below this one.
    base, _ = sm.patch(base, {"variation.amount": 0.0})
    print(f"\nPARAMETER SWEEPS on {sm.get(base, 'name')} "
          f"at {sm.get(base, 'resolution_cm')} cm, {len(seeds)} seeds "
          f"averaged, individual variation off\n")
    print(f"{'parameter':<26} {'measurement':<32} {'low':>9} {'high':>9} "
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
        print(f"{path:<26} {label:<32} {lo:>9.2f} {hi:>9.2f} "
              f"{rel * 100:>7.0f}%  {state}{note}")

    print()
    for path, choices, measure, unit, label in CHOICE_SWEEPS:
        vals = []
        setup = CHOICE_SETUP.get(path, {})
        start = sm.patch(base, setup)[0] if setup else base
        for c in choices:
            spec, _ = sm.patch(start, {path: c})
            vals.append((c, _mean(spec, measure, seeds)))
        lo = min(v for _, v in vals)
        hi = max(v for _, v in vals)
        state, rel = _verdict(lo, hi, unit)
        dead += state in ("DEAD", "ERROR")
        detail = "  ".join(f"{c}={v:.1f}" for c, v in vals)
        print(f"{path:<26} {label:<32} {rel * 100:>7.0f}%  {state}")
        print(f"{'':<26} {detail}")
    return dead


def variation(base: dict, seeds: list[int]) -> None:
    """Do two individuals of one species actually differ?

    The failure this catches is specific and has happened here before: a fish
    reads `variation.*` in `fish._params` while `spec.realize` varies the tree
    parameters, so a wiring mistake shows up as a shoal of identical animals --
    and a shoal is the one place that is unmissable and the one place nobody
    looks until it is in the game.
    """
    print("\nVARIATION across seeds (a shoal of clones is the failure)\n")
    for amount in (0.0, 1.0, 2.0):
        spec, _ = sm.patch(base, {"variation.amount": amount})
        lens = [m_length(pipeline.build(spec, s)) for s in seeds]
        deps = [m_body_depth(pipeline.build(spec, s)) for s in seeds]
        print(f"  variation.amount {amount:>4.1f}   "
              f"length {np.mean(lens):>5.1f} +/- {np.std(lens):>4.2f} vox   "
              f"body depth {np.mean(deps):>4.1f} +/- {np.std(deps):>4.2f} vox   "
              f"distinct lengths {len(set(lens))}/{len(lens)}")


# --- readability ------------------------------------------------------------
#
# The three instruments the research recommends, all mechanical, all answering
# a question a person would otherwise answer by squinting.


def _greyscale(rgb) -> float:
    r, g, b = rgb
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def _contrast_ratio(c1, c2) -> float:
    """WCAG relative-luminance contrast ratio between two preview colours."""
    def lin(c):
        c = c / 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4

    def lum(rgb):
        r, g, b = (lin(v) for v in rgb)
        return 0.2126 * r + 0.7152 * g + 0.0722 * b

    a, b = sorted((lum(c1), lum(c2)), reverse=True)
    return (a + 0.05) / (b + 0.05)


def readability(names: list[str], seeds: list[int]) -> int:
    """Silhouette, value contrast, and survival of a two-voxel blur.

    WHY VALUE AND NOT COLOUR. The eye carries brightness at about four times
    the spatial detail it carries red-against-green, which is why video has
    thrown away colour resolution for fifty years and nobody notices. A marking
    that differs from the flank only in HUE therefore blurs away roughly four
    times sooner than one that differs in BRIGHTNESS -- and at twenty voxels,
    "four times sooner" means "before the player ever sees it". A pattern can
    be perfectly present in the voxels and invisible in the water.

    So the contrast between a species' flank and its marking is checked as a
    number, and 1.5 is the floor. Below that the pattern is decoration for a
    screenshot taken from one metre away.
    """
    print("\nREADABILITY  (silhouette, value contrast, blur survival)\n")
    print(f"{'species':<22} {'len':>4} {'deep':>5} {'ink%':>6} {'contrast':>9} "
          f"{'blur keeps':>11}  flags")
    bad = 0
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        a = pipeline.build(spec, seeds[0])
        occ = _occ(a)
        length, depth = m_length(a), m_depth(a)
        ink = m_pattern(a)

        flank = materials.color(materials.resolve(sm.get(spec, "materials.fish_flank")))
        markm = materials.resolve(sm.get(spec, "materials.fish_pattern"))
        mark = materials.color(markm)
        has_pattern = (sm.get(spec, "fish.pattern") != "none"
                       or (sm.get(spec, "fish.field_curve") == "hourglass"
                           and float(sm.get(spec, "fish.curve_amount")) > 0.0))
        cr = _contrast_ratio(flank, mark) if has_pattern else float("nan")

        # Blur survival: does the SILHOUETTE still have a fish's aspect after
        # the equivalent of moving two voxels away? Measured as the share of
        # the silhouette that survives a 2-voxel erosion -- the pixel-art
        # "features under two across disappear" rule, run rather than quoted.
        sil = occ.any(axis=1)
        keep = sil.copy()
        for _ in range(1):
            k = keep.copy()
            k[:-1] &= keep[1:]
            k[1:] &= keep[:-1]
            k[:, :-1] &= keep[:, 1:]
            k[:, 1:] &= keep[:, :-1]
            keep = k
        survive = 100.0 * keep.sum() / max(sil.sum(), 1)

        flags = []
        # THE 20 cm FLOOR IS A POLICY, so it is checked against the authored
        # size rather than against the built one. Owner decision 2026-08-13:
        # rather than add a 5 mm lattice tier for the small reef fish, enlarge
        # the species. A `variation` draw can take an individual below 20
        # voxels and that is fine; a SPEC below 0.20 m is the thing that was
        # ruled out, and it is the thing a person would reintroduce by
        # "correcting" a size back to life size.
        authored = float(sm.get(spec, "fish.length_m"))
        if authored < 0.20:
            flags.append(f"UNDER THE 20 cm FLOOR (authored {authored * 100:.0f} cm)")
        if length < 18:
            flags.append(f"SHORT ({length:.0f} vox; the reference is 20-30)")
        if depth < 5:
            flags.append("SHALLOW (under 5 voxels deep)")
        if has_pattern and ink < 8:
            flags.append(f"FAINT MARKING ({ink:.1f}% of the fish)")
        if has_pattern and cr < 1.5:
            flags.append(f"LOW VALUE CONTRAST ({cr:.2f}, floor is 1.5)")
        if survive < 25:
            flags.append(f"THIN ({survive:.0f}% survives a 1-voxel erosion)")
        bad += bool(flags)
        print(f"{n:<22} {length:>4.0f} {depth:>5.0f} {ink:>6.1f} "
              f"{cr:>9.2f} {survive:>10.0f}%  {'; '.join(flags)}")
    return bad


# The voxel sizes a species may be authored at, coarsest first. Mirrors the
# `resolution_cm` choices in `forge/spec.py`; the probe checks that at import so
# it cannot sweep a size the app cannot select.
TIERS = (10.0, 5.0, 2.5, 2.0, 1.0)

# Skip a (species, tier) pair whose grid is bigger than this many cells. A blue
# whale at 1 cm is 390 MILLION cells; the point of this table is to find the
# lattice that WORKS, not to spend twenty minutes proving that the one nobody
# would choose is expensive.
LATTICE_CELL_BUDGET = 40e6

# The tier list above mirrors `resolution_cm`'s choices, and a mirror that can
# drift is this repo's documented failure mode. Checked at import: sweeping a
# size the app cannot select would report a lattice nobody can author at.
_CHOICES = {float(c) for c in sm.BY_PATH["resolution_cm"].choices}
assert set(TIERS) == _CHOICES, (
    f"fishprobe TIERS and spec.py resolution_cm disagree: "
    f"{set(TIERS) ^ _CHOICES}")


def lattice(names: list[str], seeds: list[int]) -> None:
    """Every authorable voxel size, against the features that decide it.

    THE LATTICE ARGUMENT IS NOT ABOUT COST. A 30 cm fish is free at any of
    these sizes and a 25 m whale is unaffordable at the finest, but neither
    fact decides anything on its own: what decides it is whether the FEATURES
    THAT CARRY IDENTITY still exist. So this measures the features.

    The five columns are the five things that were argued for elsewhere as
    doing the identifying, and each has a floor of about two voxels below which
    it is not there:

      len      body length in voxels -- the budget everything else spends from
      deep     maximum depth, which is what separates the shape classes
      fork     the notch in the tail, measured as enclosed empty cells
      dorsal   how far the back fin stands above the body line
      eye      eye voxels; only trustworthy where the eye material differs
               from the marking material, which is why several species read 0

    A BIG ANIMAL NEEDS MORE VOXELS OF LENGTH THAN A SMALL ONE, NOT FEWER, and
    that is the non-obvious part. Identity features do not scale with the
    animal: a blue whale's dorsal fin is about 1.5% of its length where a
    perch's is 50% of its depth, so the whale needs a much longer voxel body
    before that fin is two voxels tall. Reading this table down a column is how
    the per-species `resolution_cm` gets chosen.
    """
    print("\nLATTICE  (which voxel size keeps the features?)\n")
    print(f"{'species':<22} {'cm':>5} {'len':>5} {'deep':>5} {'fork':>5} "
          f"{'dorsal':>7} {'eye':>4} {'vox':>10}  note")
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        authored = float(sm.get(spec, "resolution_cm"))
        first = True
        for cm in TIERS:
            nx, ny, nz = render.predicted_extent(spec, cm / 100.0)
            if nx * ny * nz > LATTICE_CELL_BUDGET:
                print(f"{n if first else '':<22} {cm:>5g} "
                      + " " * 34
                      + f"  refused: {nx * ny * nz / 1e6:,.0f}M cells")
                first = False
                continue
            a = pipeline.build(spec, seeds[0], resolution_cm=cm)
            note = "<- authored here" if abs(cm - authored) < 1e-9 else ""
            print(f"{n if first else '':<22} {cm:>5g} "
                  f"{m_length(a):>5.0f} {m_depth(a):>5.0f} {m_tail_notch(a):>5.0f} "
                  f"{m_dorsal(a):>7.1f} {m_eye(a):>4.0f} "
                  f"{a.stats['voxels']:>10,}  {note}")
            first = False


# --- the three mechanisms added on 2026-08-13 -------------------------------
#
# Each gets a per-species table here as well as a row in the sweeps above,
# because the sweeps prove the PARAMETER moves something on one species and
# these prove the SPECIES that authored it actually got it. Both have failed
# separately in this repo: `fish.length_m` swept fine while every whale on disk
# was clamped to 3 m at authoring time.


def _three_fields() -> dict:
    """Repaint so back, flank and belly are three different colours.

    Every measurement below reads where one colour stops, and half the species
    here paint two roles the same -- a great white's back, eye and marking are
    all `skin_dark`, an orca's back and flank both are. Read off the authored
    colours, "where does the back end" would answer "where the flank ends" on
    exactly the animals this mechanism exists for. This changes no geometry.
    """
    return {
        "materials.fish_back": "skin_dark",
        "materials.fish_flank": "skin_silver",
        "materials.fish_belly": "skin_pale",
        "materials.fish_fin": "skin_green",
        "materials.fish_pattern": "skin_green",
        "materials.fish_eye": "skin_green",
        "materials.fish_patch": "skin_green",
        "fish.pattern": "none",
        "variation.amount": 0.0,
    }


def _flank_run(a) -> np.ndarray:
    """How many voxels of flank colour each column carries."""
    mat = materials.resolve(sm.get(a.spec, "materials.fish_flank"))
    return (a.grid.data == mat).any(axis=1).sum(axis=1)


def _curve_against_flat(spec, seed):
    """(dip, rise, waist, depth, rise-per-column) against the same animal flat.

    The fifth item is the whole profile rather than its maximum, because
    `fish.curve_at` is measured by WHERE the pale edge moved and not by how
    far. Returning only the four summary numbers is what this function did
    first, and `m_curve_where` indexed past the end of the tuple -- which
    `_mean` swallowed into a nan and the sweep table reported as ERROR. The
    probe catching its own wiring fault is the system working, and it is the
    fourth time this file has been wrong about something it measures.

    The control is the SAME SPEC with `fish.field_curve` set to `flat`, built
    with individual variation off so the two are the same animal to the voxel.
    Anything else compares two different dolphins and calls the difference a
    cape.
    """
    curved, _ = sm.patch(spec, _three_fields())
    flat, _ = sm.patch(curved, {"fish.field_curve": "flat"})
    a, b = pipeline.build(curved, seed), pipeline.build(flat, seed)
    dip = _column_line(b, "back", upward=False) - _column_line(a, "back", upward=False)
    rise = _column_line(a, "belly", upward=True) - _column_line(b, "belly", upward=True)
    fa, fb = _flank_run(a), _flank_run(b)
    n = fb.size
    mid = slice(int(0.10 * n), int(0.90 * n))
    # The waist is how much of the flank the two curves pinched out where they
    # meet, so it is measured where the FLAT animal still had a flank to lose.
    live = fb[mid] > 0
    waist = float((fb[mid][live] - fa[mid][live]).max()) if live.any() else 0.0
    sil = (b.grid.data != 0).any(axis=1)
    depth = float(np.median(sil[mid].sum(axis=1)))
    return (float(np.nanmax(dip)) if np.isfinite(dip).any() else 0.0,
            float(np.nanmax(rise)) if np.isfinite(rise).any() else 0.0,
            waist, depth, rise)


def marks(names: list[str], seeds: list[int]) -> int:
    """Does each species' shaped boundary actually bend, and by how many voxels?

    THE LATTICE IS WHAT KILLS THIS FEATURE, NOT THE CODE. A boundary that moves
    by under two voxels is not a curve, it is a ragged line, and whether the
    authored amount clears two voxels depends entirely on how deep the animal
    is in voxels -- which runs from 7 on a river perch to 40 on a blue whale.
    So the `depth` and `2vox needs` columns are printed for EVERY species,
    curved or not: they say which species could carry one at all.
    """
    print("\nSHAPED COLOUR BOUNDARIES  (against the same animal drawn flat)\n")
    print(f"{'species':<22} {'curve':<10} {'amt':>5} {'depth':>6} {'2vox':>6} "
          f"{'dip':>5} {'rise':>5} {'waist':>6}  flags")
    bad = 0
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        shape = str(sm.get(spec, "fish.field_curve"))
        amount = float(sm.get(spec, "fish.curve_amount"))
        dip, rise, waist, depth, _ = _curve_against_flat(spec, seeds[0])
        need = 2.0 / max(depth, 1e-6)
        flags = []
        if shape != "flat" and amount > 0.0:
            moved = max(dip if shape in ("cape", "hourglass") else 0.0,
                        rise if shape in ("flame", "hourglass") else 0.0)
            if moved < 2.0:
                flags.append(f"BOUNDARY MOVES {moved:.0f} VOXELS (floor is 2; "
                             f"this body needs amount >= {need:.2f})")
            if shape in ("cape", "hourglass") and dip < 2.0:
                flags.append(f"cape dip only {dip:.0f} vox")
            if shape in ("flame", "hourglass") and rise < 2.0:
                flags.append(f"flame rise only {rise:.0f} vox")
            if shape == "hourglass" and waist < 1.0:
                flags.append("hourglass never pinches the flank")
        elif dip or rise or waist:
            # A flat animal that moved is a wiring fault, not a tuning one.
            flags.append(f"FLAT BUT MOVED (dip {dip:.0f}, rise {rise:.0f})")
        bad += bool(flags)
        print(f"{n:<22} {shape:<10} {amount:>5.2f} {depth:>6.0f} {need:>6.2f} "
              f"{dip:>5.0f} {rise:>5.0f} {waist:>6.0f}  {'; '.join(flags)}")
    return bad


def head(names: list[str], seeds: list[int]) -> int:
    """The cephalofoil: how wide is the head, and is the eye out on the tip?

    The published figure is a share of TOTAL length, so that is the column to
    read: a scalloped hammerhead is 25-32%.
    """
    print("\nHEAD SPAN  (the cephalofoil)\n")
    print(f"{'species':<22} {'authored':>9} {'total':>6} {'head':>5} {'body':>5} "
          f"{'head %TL':>9} {'chord':>6} {'thick':>6} {'t/span':>7} {'eye out':>8}"
          f"  flags")
    bad = 0
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        hw = float(sm.get(spec, "fish.head_width"))
        # Repainted so the eye is the only red thing on the animal. Read off
        # the authored colours the eye column measured the whole back on every
        # species whose eye and back are both `skin_dark` -- a whale shark came
        # back 99 voxels "wide at the eye" on a 37-voxel head.
        probe, _ = sm.patch(spec, {**_three_fields(),
                                   "materials.fish_eye": "skin_red"})
        a = pipeline.build(probe, seeds[0])
        plan = _occ(a).any(axis=2)
        nx = plan.shape[0]
        span = m_head_span(a)
        # The body's own width, taken behind the head and in front of the
        # pectorals, so the comparison is head against body rather than head
        # against fin.
        body = float(np.median(plan[int(0.40 * nx):int(0.60 * nx)].sum(axis=1)))
        # Chord and thickness, both against published ratios. THE WINDOW STOPS
        # AT THE FRONT FIFTH: the pectoral fins insert at 0.28 of the body and
        # stick out further than anything else on a shark, so a chord measured
        # over the front quarter came back at 0.52 of the span -- it had
        # measured the fins. The front seventh is inside every head_frac here.
        occ = _occ(a)
        front = slice(int(0.86 * nx), nx)
        wide = plan[front].sum(axis=1) > body + 2
        chord = int(wide.sum())
        xw = int(np.argmax(plan[front].sum(axis=1))) + front.start
        thick = int(occ[xw].any(axis=0).sum())
        # Is the eye out on the tip of the wing? On a hammerhead it is, and it
        # is not drawn there -- `_eye` puts the pupil on the outermost occupied
        # voxel at its station, so widening the head carries the eye out with
        # it for free. Worth measuring precisely because it is free: a change
        # that broke it would be invisible.
        mat = materials.resolve(sm.get(spec, "materials.fish_eye"))
        ys = np.nonzero((a.grid.data == mat).any(axis=2))[1]
        eye_out = float(ys.max() - ys.min() + 1) if ys.size else 0.0
        flags = []
        if hw > 0.0:
            if span < body + 3:
                flags.append(f"HEAD NOT WIDER THAN THE BODY ({span:.0f} vs {body:.0f})")
            if eye_out < span - 2:
                flags.append(f"eye is {span - eye_out:.0f} voxels inboard of the tip")
            # A wing, not a slab. The only measured number is the Sphyrna
            # gilberti holotype's 0.094; anything past twice that is a lump.
            if thick > 0.20 * span:
                flags.append(f"HEAD IS A SLAB ({thick} thick on {span:.0f} of "
                             f"span = {thick / max(span, 1):.2f}; the measured "
                             f"animal is 0.09)")
        bad += bool(flags)
        # THE WING COLUMNS ARE BLANK WHERE THERE IS NO WING. A sperm whale has
        # no cephalofoil and its head is a quarter of the animal, so the same
        # arithmetic returns a "thickness over span" of 1.12 -- a real number
        # measuring nothing, printed under a heading that implies otherwise,
        # which is how a table teaches somebody a wrong fact.
        wing = (f"{chord:>6} {thick:>6} {thick / max(span, 1):>7.2f}"
                if hw > 0.0 else f"{'--':>6} {'--':>6} {'--':>7}")
        print(f"{n:<22} {hw:>9.2f} {nx:>6} {span:>5.0f} {body:>5.0f} "
              f"{100.0 * span / max(nx, 1):>8.1f}% {wing} {eye_out:>8.0f}  "
              f"{'; '.join(flags)}")
    return bad


def sexes(names: list[str], seeds: list[int]) -> int:
    """Male against female, per species, in voxels.

    THE POINT OF THIS TABLE IS THE SPECIES THAT DO NOT MOVE. Most of this
    library has no dimorphism worth drawing at these sizes, and the honest
    output is a column of zeros with the ratios at 1.00 -- not an invented
    difference. What it has to catch is the other case: a species that AUTHORS
    a ratio and does not get it, which is the silent no-op wearing a
    biologist's hat.

    Three things are checked and each fails on its own:

      * the measured male-to-female difference in voxels, which has to clear
        two on any species that claims a ratio at all;
      * that `unsexed` really is the average of the two -- the authored numbers
        are the mean and the ratio is split as a square root either way, so a
        version that scaled only the male would show up here as an unsexed
        animal identical to one of the sexes;
      * that a male and a female of one seed are DIFFERENT INDIVIDUALS. That is
        the design decision and it is the opposite of `bird.pose`, so it is
        checked rather than assumed.
    """
    print("\nSEX  (male against female, individual variation off)\n")
    print(f"{'species':<22} {'ratios L/D/P':<18} {'len f/m':>12} "
          f"{'dorsal f/m':>12} {'pect f/m':>11} {'moved':>6}  flags")
    bad = 0
    for n in names:
        spec, _ = sm.load(SPECS / f"{n}.json")
        pinned, _ = sm.patch(spec, {"variation.amount": 0.0})
        rl = float(sm.get(spec, "fish.sex_length"))
        rd = float(sm.get(spec, "fish.sex_dorsal"))
        rp = float(sm.get(spec, "fish.sex_pectoral"))
        built = {}
        for s in ("unsexed", "female", "male"):
            built[s] = pipeline.build(sm.patch(pinned, {"fish.sex": s})[0], seeds[0])
        L = {k: m_length(v) for k, v in built.items()}
        D = {k: m_dorsal(v) for k, v in built.items()}
        P = {k: m_pectoral(v) for k, v in built.items()}
        moved = max(abs(L["male"] - L["female"]), abs(D["male"] - D["female"]),
                    abs(P["male"] - P["female"]))
        claims = max(abs(rl - 1.0), abs(rd - 1.0), abs(rp - 1.0)) > 0.01
        flags = []
        if claims and moved < 2.0:
            flags.append(f"CLAIMS DIMORPHISM AND MOVES {moved:.0f} VOXELS")
        if not claims and moved > 0.0:
            flags.append(f"NO RATIO AUTHORED BUT MOVED {moved:.0f} VOXELS")
        # The average has to sit between the two sexes on every measurement
        # that moved at all.
        for label, m in (("length", L), ("dorsal", D), ("pectoral", P)):
            lo, hi = sorted((m["female"], m["male"]))
            if hi - lo >= 2.0 and not (lo - 1.0 <= m["unsexed"] <= hi + 1.0):
                flags.append(f"unsexed {label} {m['unsexed']:.0f} is outside "
                             f"[{lo:.0f}, {hi:.0f}]")
        if claims:
            hm = sm.seed_hash(sm.patch(spec, {"fish.sex": "male"})[0])
            hf = sm.seed_hash(sm.patch(spec, {"fish.sex": "female"})[0])
            if hm == hf:
                flags.append("male and female seed to the SAME individual; sex "
                             "is meant to reseed -- see fish.sex")
        bad += bool(flags)
        print(f"{n:<22} {rl:.2f}/{rd:.2f}/{rp:.2f}      "
              f"{L['female']:>5.0f}/{L['male']:<6.0f} "
              f"{D['female']:>5.0f}/{D['male']:<6.0f} "
              f"{P['female']:>4.0f}/{P['male']:<6.0f} {moved:>6.0f}  "
              f"{'; '.join(flags)}")
    return bad


# --- the renders ------------------------------------------------------------
#
# A measurement says the mechanism moved voxels. It cannot say the result reads
# as an animal, and nothing here ever will: the owner judges the picture. These
# three write the sheets that question gets asked from.


def _sheet(rows, out: Path, cell_w: int = 460, cell_h: int = 190) -> Path:
    """One row per case, one column per variant, drawn at a common scale.

    ONE SCALE PER ROW rather than per sheet. A 25 cm anemonefish beside an 11 m
    whale shark on one scale is a whale shark and a dot; per row, each pair is
    still comparable to each other, which is the only comparison an A/B is for.

    The cell is WIDE AND SHORT because these animals are. A square cell sized
    to hold a fish's length spends four fifths of its height on background, and
    the first sheet drawn that way put a 25-voxel-wide cephalofoil inside a
    thirty-pixel-tall picture.
    """
    from PIL import Image, ImageDraw
    width = max(len(v) for _, v in rows)
    sheet = Image.new("RGB", (width * cell_w + 200, len(rows) * (cell_h + 26)),
                      (24, 25, 28))
    d = ImageDraw.Draw(sheet)
    for r, (label, built) in enumerate(rows):
        y = r * (cell_h + 26)
        for i, line in enumerate(label.split("\n")):
            d.text((8, y + cell_h // 2 - 6 + 12 * i), line, (235, 238, 244))
        row_max = max(max(a.grid.shape) * a.grid.voxel_m for _, a, _ in built)
        for c, (tag, a, cam) in enumerate(built):
            metres = max(a.grid.shape) * a.grid.voxel_m
            px = max(48, int((cell_w - 24) * metres / row_max))
            img = render.view(a.grid, cam, target_px=px)
            # A deep-bodied animal drawn to a length budget can still be taller
            # than the cell -- an anemonefish is nearly as deep as it is long,
            # and a whale shark's flukes stand up out of the frame. Shrink to
            # fit rather than let it run over the row below it.
            if img.height > cell_h:
                img = img.resize((max(1, img.width * cell_h // img.height), cell_h))
            x = 200 + c * cell_w
            sheet.paste(img, (x + (cell_w - img.width) // 2,
                              y + max(0, (cell_h - img.height) // 2)), img)
            d.text((x + 8, y + cell_h + 6),
                   f"{tag}   {a.stats['voxels']:,} vox   "
                   f"{a.stats['length_m']:.2f} m   {cam}", (198, 202, 210))
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    return out


def head_ab(seed: int) -> Path:
    """Before and after on the hammerhead, from both cameras.

    BOTH CAMERAS ON PURPOSE. The head span is a plan-view feature and the fish
    review camera sits at 8 degrees, so the left pair is the comparison the
    gallery used to show and the right pair is what `render.camera_for` now
    sends a wide-headed fish to.
    """
    spec, _ = sm.load(SPECS / "scalloped-hammerhead.json")
    rows = []
    for cam in ("broad", "broadhigh"):
        built = []
        for tag, hw in (("no head span (before)", 0.0), ("head span 0.38", 0.38)):
            s, _ = sm.patch(spec, {"fish.head_width": hw, "variation.amount": 0.0})
            built.append((f"{tag}", pipeline.build(s, seed), cam))
        rows.append((f"hammerhead, {cam} camera", built))
    return _sheet(rows, ROOT / "out" / "fish" / "hammerhead-ab.png")


def marks_ab(seed: int) -> Path:
    """Each shaped boundary against the same animal drawn flat.

    THE FIRST TWO COLUMNS HAVE THE MARKING TURNED OFF and the third is the
    species as authored. Not tidiness: the orca wears a `saddle`, whose blotch
    field is seeded from the spec hash, and `fish.field_curve` is part of that
    hash -- so the flat column and the flame column came out with the saddle in
    two different places and the reader could not tell which difference was the
    boundary. Turning the marking off in both makes the pair differ in exactly
    one thing, which is what an A/B is.
    """
    rows = []
    for name in ("bottlenose-dolphin", "orca", "common-dolphin"):
        spec, _ = sm.load(SPECS / f"{name}.json")
        cam = render.camera_for(spec)
        pinned, _ = sm.patch(spec, {"variation.amount": 0.0})
        plain, _ = sm.patch(pinned, {"fish.pattern": "none"})
        flat, _ = sm.patch(plain, {"fish.field_curve": "flat"})
        curve = str(sm.get(spec, "fish.field_curve"))
        rows.append((f"{name}\n{curve} {sm.get(spec, 'fish.curve_amount'):g} "
                     f"at {sm.get(spec, 'fish.curve_at'):g}", [
            ("flat, no marking", pipeline.build(flat, seed), cam),
            (f"{curve}, no marking", pipeline.build(plain, seed), cam),
            ("as authored", pipeline.build(pinned, seed), cam),
        ]))
    return _sheet(rows, ROOT / "out" / "fish" / "boundary-ab.png", cell_w=400)


def sex_ab(seed: int) -> Path:
    """Female, average and male, on the species that move most.

    DRAWN WITH INDIVIDUAL VARIATION OFF, and that is the opposite of what
    `tools/birdprobe.py --pose-ab` does. A pose A/B has to leave variation ON,
    because the defect it hunts is the two poses coming out as two different
    birds. Here two different animals is the DESIGN -- a male orca and a female
    orca are not one whale in two postures -- so leaving the draw in would show
    a size difference that is partly the dimorphism and partly the dice, and
    the reader could not tell which. The reseeding is checked as a hash by
    `--sex` instead, where it can be stated exactly.
    """
    rows = []
    for name in ("orca", "whale-shark", "sperm-whale", "clown-anemonefish"):
        spec, _ = sm.load(SPECS / f"{name}.json")
        cam = render.camera_for(spec)
        pinned, _ = sm.patch(spec, {"variation.amount": 0.0})
        built = []
        for sex in ("female", "unsexed", "male"):
            s, _ = sm.patch(pinned, {"fish.sex": sex})
            built.append((sex, pipeline.build(s, seed), cam))
        ratios = (f"L {sm.get(spec, 'fish.sex_length'):g}  "
                  f"D {sm.get(spec, 'fish.sex_dorsal'):g}  "
                  f"P {sm.get(spec, 'fish.sex_pectoral'):g}")
        rows.append((f"{name}\n{ratios}", built))
    return _sheet(rows, ROOT / "out" / "fish" / "sex-ab.png")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--spec", default="brown-trout",
                    help="which species the sweeps are run on (default brown-trout)")
    ap.add_argument("--seeds", type=int, default=4,
                    help="how many individuals to average each point over")
    ap.add_argument("--read", action="store_true", help="readability tests only")
    ap.add_argument("--lattice", action="store_true", help="lattice comparison only")
    ap.add_argument("--marks", action="store_true",
                    help="shaped colour boundaries, per species, in voxels")
    ap.add_argument("--head", action="store_true",
                    help="head span (the cephalofoil), per species")
    ap.add_argument("--sex", action="store_true",
                    help="male against female, per species, in voxels")
    ap.add_argument("--ab", action="store_true",
                    help="write the three A/B render sheets into out/fish/")
    args = ap.parse_args()

    seeds = list(range(1, args.seeds + 1))
    # BOTH swimming kinds. This read `== "fish"` and quietly reported on ten
    # species while twelve more sat on disk unmeasured -- the lattice table,
    # which exists to justify a whale's voxel size, had no whale in it.
    names = sorted(p.stem for p in SPECS.glob("*.json")
                   if sm.get(sm.load(p)[0], "kind") in ("fish", "cetacean"))
    if not names:
        print("no fish specs found; run tools/seed_fish.py", file=sys.stderr)
        return 2

    if args.lattice:
        lattice(names, seeds)
        return 0
    if args.read:
        return 1 if readability(names, seeds) else 0
    if args.marks:
        return 1 if marks(names, seeds) else 0
    if args.head:
        return 1 if head(names, seeds) else 0
    if args.sex:
        return 1 if sexes(names, seeds) else 0
    if args.ab:
        for fn in (head_ab, marks_ab, sex_ab):
            print(fn(seeds[0]))
        return 0

    base, _ = sm.load(SPECS / f"{args.spec}.json")
    dead = sweep(base, seeds)
    variation(base, seeds)
    bad = readability(names, seeds)
    bad += marks(names, seeds)
    bad += head(names, seeds)
    bad += sexes(names, seeds)
    print(f"\n{dead} parameter(s) measured as DEAD; "
          f"{bad} species carry a readability flag.")
    print("A DEAD parameter is not a tuning problem. It is a wiring problem.")
    return 1 if dead else 0


if __name__ == "__main__":
    raise SystemExit(main())
