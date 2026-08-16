"""Ground cover: grass tufts, reed clumps and small flowering plants.

These three are one shape with three settings, not three generators. A grass
tuft, a stand of reeds and a clump of daisies are all *a spray of thin stems
rising from a common root*; what separates them is how tall the stems are, how
far they arc over, and what sits on top — nothing, a seed spike, or a bloom.
Writing them as one generator is not a shortcut, it is the actual shape.

None of it uses the tree machinery. A blade of grass does not branch, does not
compete for space and does not carry foliage clumps, so space colonization has
nothing to contribute and every trunk/crown/growth slider would be dead weight.

Two things about this at voxel scale are worth knowing before reading the code.

**These assets only make sense at 2 cm.** A grass tuft is 20-40 cm tall, so at
the terrain's 10 cm it is three voxels and there is nothing to draw. At 2 cm it
is 15-20 voxels tall with stems one voxel thick, which is exactly the scale the
2 cm lattice was added for.

**A stem one voxel thick is drawn by its centreline.** `grid.capsule` already
falls back to a face-connected voxel run when the radius is under half a voxel,
which is the whole blade at this size. That is why these come out connected
rather than dotted.
"""

from __future__ import annotations

import math

import numpy as np

from . import materials
from .grid import VoxelGrid, m_to_vox
from .spec import BY_PATH, get

_UP = np.array([0.0, 0.0, 1.0])

# THE STEM MENU AND THE HEAD MENU ARE THE SAME MENU, AND THIS IS WHERE THAT IS
# CHECKED, because this is the file that resolves both of them.
#
# They used to differ -- seven entries against fourteen, overlapping in four --
# and a head material that was only in the stem list came back as
# `leaf_blossom`, a pink, with no error anywhere. `docs/aquatic-species.md`
# §8.6a has the four species that shipped that way. The menus are one tuple in
# `spec.py` now, and this asserts they still are: widening one and forgetting
# the other is a two-line change that would reopen the same hole, and it would
# reopen it SILENTLY, which is the only reason this is an assert at import
# rather than a note in a docstring.
#
# Same guard, same reasoning, as `envelope.py` on crown shapes and
# `rasterize.py` on foliage habits. Both of those were written after a choice
# fell through to a default and nobody could see it for months.
assert BY_PATH["materials.stem"].choices == BY_PATH["materials.head"].choices, (
    "forge/spec.py: materials.stem and materials.head no longer offer the same "
    "menu; an out-of-menu choice is replaced with the default, silently, and "
    "the default head material is a pink -- see docs/aquatic-species.md 8.6a")
assert all(n in materials.BY_NAME for n in BY_PATH["materials.stem"].choices), (
    "forge/spec.py: the plant material menu names something "
    "forge/materials.py cannot resolve")


def build(spec: dict, rng: np.random.Generator, voxel_m: float) -> VoxelGrid:
    stems = max(1, int(get(spec, "tuft.stems")))
    height = float(get(spec, "height_m"))
    spread = float(get(spec, "tuft.spread_m"))
    splay = math.radians(float(get(spec, "tuft.splay_deg")))
    arc = float(get(spec, "tuft.arc"))
    width = float(get(spec, "tuft.width_m"))
    taper = float(get(spec, "tuft.taper"))
    wander = float(get(spec, "tuft.wander"))
    lvar = float(get(spec, "tuft.length_var"))
    head = str(get(spec, "tuft.head"))
    head_m = float(get(spec, "tuft.head_m"))
    head_frac = float(get(spec, "tuft.head_frac"))
    head_share = float(get(spec, "tuft.head_share"))
    base_m = float(get(spec, "tuft.base_m"))
    stem_mat = materials.resolve(get(spec, "materials.stem"))
    head_mat = materials.resolve(get(spec, "materials.head"))

    # 1. Trace every stem in METRES first, then size the grid from what was
    #    actually traced. A tuft's reach depends on how far its stems arc over,
    #    which is not worth predicting when the points are already in hand.
    paths: list[np.ndarray] = []
    carries_head: list[bool] = []
    for i in range(stems):
        paths.append(_stem(rng, i, stems, height, spread, splay, arc, wander,
                           lvar, voxel_m))
        carries_head.append(head != "none" and rng.random() < head_share)

    pad = width * 0.5 + (head_m if head != "none" else 0.0) + voxel_m * 2.0
    allpts = np.concatenate(paths, axis=0)
    lo = allpts.min(axis=0) - pad
    hi = allpts.max(axis=0) + pad
    lo[2] = 0.0                      # the tuft sits ON the ground, not in it

    shape = tuple(max(3, int(m_to_vox(hi[a] - lo[a], voxel_m)) + 2) for a in range(3))
    grid = VoxelGrid(shape, (0, 0, 0), voxel_m)

    def to_vox(p: np.ndarray) -> np.ndarray:
        return m_to_vox(p - lo, voxel_m)

    # 2. A root crown joining every stem.
    #
    #    Without it each stem is its own connected component -- botanically
    #    true, and wrong for an asset that gets stamped into a world and dug out
    #    of it, where one asset should be one piece. It is also what the health
    #    check means by "loose voxels", so a tuft without it reads as broken.
    if base_m > 0.0:
        c = to_vox(np.array([0.0, 0.0, 0.0]))
        c[2] = 0.0
        # It has to REACH every stem, so it is never smaller than the disc the
        # stems root in. Left as an independent slider it silently failed on any
        # wide stand -- a reed clump rooting over 18 cm with a 5 cm crown came
        # out 39% detached, which looks like a generator bug and is a unit
        # mismatch between two sliders that were never tied together.
        r = max(base_m, spread + width * 0.5)
        _pad(grid, c, m_to_vox(r, voxel_m), stem_mat)

    # 3. Draw.
    r0 = m_to_vox(width * 0.5, voxel_m)
    r1 = max(r0 * taper, 0.0)
    for path, has_head in zip(paths, carries_head):
        pts = to_vox(path)
        n = len(pts) - 1
        for k in range(n):
            t0, t1 = k / n, (k + 1) / n
            grid.capsule(pts[k], pts[k + 1],
                         r0 + (r1 - r0) * t0, r0 + (r1 - r0) * t1, stem_mat)
        if has_head:
            # head_m is the head's WIDTH, so half of it is the radius the
            # drawing code wants. Passing the whole thing made every bloom twice
            # the size the number said and put 24 cm daisies in a meadow.
            _head(grid, pts, head, m_to_vox(head_m * 0.5, voxel_m),
                  head_frac, head_mat, rng)

    return grid


def _stem(rng, index, total, height, spread, splay, arc, wander, lvar,
          voxel_m) -> np.ndarray:
    """One blade or stem, as a polyline in metres with its root at z=0.

    The angle from vertical is a function of how far along the stem you are, not
    a rotation integrated step by step: it starts at the splay angle and opens
    toward horizontal as `arc` dictates, weighted so the tip droops much further
    than the base. That is the shape of a real blade, and it also means `arc` is
    a property of the stem rather than an accumulation of round-off.
    """
    # Azimuths are STRATIFIED, not drawn independently. Independent draws clump
    # -- that is what random looks like -- and on a plant with eleven stems the
    # clumping is the whole silhouette: the first flowers came out with every
    # bloom bunched on one side and a bald gap opposite, which reads as a broken
    # generator rather than as natural variation. One stem per even slice, with
    # enough jitter to stay irregular, fixes it without making a fan.
    az = 2.0 * math.pi * (index + 0.5 + 0.85 * (rng.random() - 0.5)) / max(total, 1)
    root_r = spread * math.sqrt(rng.random())
    p = np.array([math.cos(az) * root_r, math.sin(az) * root_r, 0.0])

    length = max(height * (1.0 + lvar * (rng.random() * 2.0 - 1.0)), voxel_m * 3.0)
    # Per-stem variation, so a tuft is not a fan of identical copies.
    sp = splay * (0.3 + 0.7 * rng.random())
    ar = min(1.0, arc * (0.55 + 0.8 * rng.random()))

    steps = max(4, min(80, int(length / max(voxel_m * 1.2, 1e-6))))
    ds = length / steps
    out_az = az if root_r > 1e-9 else rng.random() * 2.0 * math.pi

    pts = [p.copy()]
    for k in range(steps):
        t = (k + 0.5) / steps
        out_az += wander * (rng.random() * 2.0 - 1.0) * 0.4
        # Vertical at the root, opening toward horizontal by the tip.
        ang = sp + ar * (math.pi * 0.5 - sp) * (t ** 1.4)
        d = np.array([math.cos(out_az) * math.sin(ang),
                      math.sin(out_az) * math.sin(ang),
                      math.cos(ang)])
        p = p + d * ds
        if p[2] < 0.0:
            p[2] = 0.0          # a blade that arcs past horizontal rests, not sinks
        pts.append(p.copy())
    return np.asarray(pts)


def _pad(grid: VoxelGrid, c: np.ndarray, r_vox: float, mat: int) -> None:
    """A flat disc of root crown at the base, two voxels tall at most."""
    r = max(r_vox, 1.0)
    nx, ny, nz = grid.shape
    x0, x1 = max(int(c[0] - r) - 1, 0), min(int(c[0] + r) + 2, nx)
    y0, y1 = max(int(c[1] - r) - 1, 0), min(int(c[1] + r) + 2, ny)
    z1 = min(2, nz)
    if x0 >= x1 or y0 >= y1 or z1 <= 0:
        return
    xs = (np.arange(x0, x1) + 0.5 - c[0]) / r
    ys = (np.arange(y0, y1) + 0.5 - c[1]) / r
    inside = (xs[:, None] ** 2 + ys[None, :] ** 2) <= 1.0
    block = grid.data[x0:x1, y0:y1, 0:z1]
    block[np.broadcast_to(inside[:, :, None], block.shape) & (block == 0)] = mat


def _head(grid: VoxelGrid, pts: np.ndarray, kind: str, r_vox: float,
          frac: float, mat: int, rng) -> None:
    """Whatever tops the stem. This is what tells the three kinds apart."""
    if r_vox <= 0.0 or len(pts) < 2:
        return
    tip = pts[-1]

    if kind == "bloom":
        # A rosette of petals around a centre, NOT a ball.
        #
        # A flower head is a few voxels across at best. A sphere at that size has
        # no roundness left to lose -- squashing one gave a flat rectangular
        # slab, which is what a bloom looked like in the first render: a pink
        # plate on a stick. Placing discrete petals on a ring keeps a shape the
        # eye reads as a flower, because the silhouette is lobed instead of
        # square.
        if r_vox < 1.9:
            # Below about two voxels the ring itself is under one voxel wide, so
            # every petal rounds onto the centre and the rosette collapses back
            # into the blob it was supposed to replace. At this size a flower
            # has to be drawn on the lattice rather than sampled onto it: one
            # centre voxel and four cardinal petals, which is the smallest thing
            # that still reads as a flower and the reason a 5 cm lattice can
            # carry a daisy at all instead of only a sunflower.
            grid.ball(tip, 0.6, mat)
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                grid.ball(tip + np.array([dx, dy, 0.0]), 0.6, mat)
            return

        petals = 5 if r_vox < 2.6 else 6 if r_vox < 3.4 else 8
        ring = r_vox * 0.62
        petal_r = max(r_vox * 0.42, 0.9)
        phase = rng.random() * 2.0 * math.pi
        grid.ball(tip, max(r_vox * 0.42, 0.9), mat)
        for k in range(petals):
            a = phase + 2.0 * math.pi * k / petals
            c = tip + np.array([math.cos(a) * ring, math.sin(a) * ring,
                                -r_vox * 0.12])
            grid.ball(c, petal_r, mat)

    elif kind == "spike":
        # A reed's seed head: the top slice of the stem, thickened.
        start = max(1, int(len(pts) * (1.0 - frac)))
        for k in range(start, len(pts) - 1):
            t = (k - start) / max(len(pts) - 1 - start, 1)
            # Fattest in the middle, tapering to a point at both ends.
            r = r_vox * math.sin(math.pi * min(max(t, 0.0), 1.0)) ** 0.6
            grid.capsule(pts[k], pts[k + 1], max(r, 0.5), max(r, 0.5), mat)

    elif kind == "plume":
        # A feathery seed head: short filaments fanning off the top of the stem.
        #
        # ANCHORED ON THE STEM'S OWN POLYLINE, not on the chord between its
        # base and its tip. Those are the same line only on a straight stem,
        # and `_stem` arcs and wanders by design -- so on anything tall and
        # curved the chord leaves the stem and the filament starts in open
        # water. `giant-kelp` is 28 m with an arc of 0.14, which is far enough
        # for exactly that: seed 3 shipped 16 voxels in two loose pieces, 83%
        # of the way up the asset, and seeds 1, 2 and 4 through 8 were clean.
        # A one-in-eight failure is what a single-seed check cannot see, and
        # this was found by the first three-seed sweep after the library grew
        # past a hundred specs.
        start = max(1, int(len(pts) * (1.0 - frac)))
        span = float(np.linalg.norm(pts[-1] - pts[start])) or 1.0
        for _ in range(14):
            a = rng.random() * 2.0 * math.pi
            t = 0.25 + 0.75 * rng.random()
            # Walk the polyline itself: the fractional index between `start`
            # and the tip, interpolated between the two points either side.
            f = start + t * (len(pts) - 1 - start)
            i = min(int(f), len(pts) - 2)
            anchor = pts[i] + (pts[i + 1] - pts[i]) * (f - i)
            reach = r_vox * (0.4 + 0.9 * rng.random())
            end = anchor + np.array([math.cos(a) * reach, math.sin(a) * reach,
                                     span * 0.18 * (rng.random() - 0.35)])
            grid.line(anchor, end, mat)
