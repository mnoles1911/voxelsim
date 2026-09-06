"""Bamboo bushcraft raft based on the owner's lake-raft photograph.

Every part is authored directly on the same cubic lattice. Bamboo consists of
thick hollow culms with solid node diaphragms, not wood logs painted with rings.
"""
import math
import numpy as np

from . import materials
from .grid import VoxelGrid
from .raft import _noise
from .spec import get


def build(spec, rng, voxel_m, steps):
    from .artifact import _axes, _shape_for, _to_vox, _polyline

    L = float(get(spec, 'artifact.length_m'))
    B = float(get(spec, 'artifact.beam_m'))
    D = float(get(spec, 'artifact.depth_m'))
    post_h = float(get(spec, 'artifact.frame_drop_m'))
    cross_r = float(get(spec, 'artifact.thwart_t_m')) / 2
    post_r = float(get(spec, 'artifact.frame_r_m'))
    margin = float(get(spec, 'artifact.deck_frac'))
    stagger = float(get(spec, 'artifact.asym_vox')) * voxel_m
    pale = materials.resolve(get(spec, 'materials.hull'))
    warm = materials.resolve(get(spec, 'materials.frame'))
    grey = materials.resolve(get(spec, 'materials.strake'))
    rope = materials.resolve(get(spec, 'materials.trim'))
    node_mat = materials.MAT_HEARTWOOD
    rope_r = voxel_m * 0.65
    deck_z = D + 0.04
    cross_z = deck_z + D / 2 + cross_r * 0.65
    stations = (margin * L, (1 - margin) * L)
    pad = 0.32
    lo = np.array([-stagger - pad, -B / 2 - pad, -0.10])
    hi = np.array([L + stagger + pad, B / 2 + pad,
                   max(deck_z + post_h, cross_z + 0.18) + 0.14])
    shape = _shape_for(lo, hi, voxel_m)
    grid = VoxelGrid(shape, (0, 0, 0), voxel_m)
    X, Y, Z = _axes(lo, shape, voxel_m)
    metrics = []

    def bamboo(axis, start, end, centre, radius, *, tone=None, hollow=True, lean=0.0):
        """One gently bowed culm with independently staggered bamboo nodes."""
        bounds0 = np.array(centre, dtype=float) - radius * 1.4 - 2 * voxel_m
        bounds1 = np.array(centre, dtype=float) + radius * 1.4 + 2 * voxel_m
        bounds0[axis], bounds1[axis] = start - voxel_m, end + voxel_m
        # Upright lean is in x and remains within the allocated local bounds.
        bounds0[0] -= abs(lean)
        bounds1[0] += abs(lean)
        low = np.maximum(0, np.floor((bounds0 - lo) / voxel_m).astype(int))
        high = np.minimum(shape, np.ceil((bounds1 - lo) / voxel_m).astype(int) + 1)
        sl = tuple(slice(int(a), int(b)) for a, b in zip(low, high))
        coordinates = [X[sl[0], :, :], Y[:, sl[1], :], Z[:, :, sl[2]]]
        t = coordinates[axis]
        f = np.clip((t - start) / (end - start), 0, 1)
        cross = [a for a in range(3) if a != axis]
        bend = rng.uniform(-0.009, 0.009) * np.sin(math.pi * f)
        u = coordinates[cross[0]] - centre[cross[0]] - bend - lean * f
        v = coordinates[cross[1]] - centre[cross[1]]
        radial = np.sqrt(u * u + v * v)
        taper = rng.uniform(-0.07, 0.07)
        r = radius * (1 + taper * (2 * f - 1))
        node_positions = []
        position = start + rng.uniform(0.12, 0.42)
        while position < end - 0.06:
            node_positions.append(position)
            position += rng.uniform(0.32, 0.49)
        ring = np.zeros(t.shape, bool)
        for position in node_positions:
            ring |= np.abs(t - position) < voxel_m * 0.55
        outer = radial <= r + ring * voxel_m * 0.30
        inner = radial < np.maximum(r - voxel_m * 1.3, voxel_m * 0.7)
        solid = outer & (t >= start) & (t <= end)
        if hollow:
            solid &= ~inner | ring
        local = grid.data[sl]
        local[solid] = pale if tone is None else tone
        salt = int(rng.integers(1, 100000000))
        # Broad sun-bleached and rubbed areas, not outlined stripes.
        stain = _noise(t / 0.62, u / 0.07, v / 0.07, salt)
        local[solid & (stain > 0.65)] = grey
        local[solid & (stain < 0.25)] = warm
        local[solid & ring & (radial > r - voxel_m)] = node_mat
        # Exposed cut rims reveal the warmer inner bamboo wall.
        cut = (t < start + voxel_m) | (t > end - voxel_m)
        local[solid & cut] = warm
        metrics.append({'axis': axis, 'length_m': float(end - start),
                        'diameter_m': float(2 * radius), 'nodes': len(node_positions)})

    n = max(5, int(round(B / (D * 0.95))))
    radii = D / 2 * rng.uniform(0.88, 1.12, n)
    centres = np.zeros(n)
    for i in range(1, n):
        centres[i] = centres[i - 1] + (radii[i - 1] + radii[i]) * 0.94
    factor = B / (centres[-1] + radii[0] + radii[-1])
    centres *= factor
    radii *= factor
    centres -= (centres[-1] + radii[-1] - radii[0]) / 2
    heights = deck_z + rng.uniform(-0.009, 0.009, n)

    def draw_deck():
        for i, y in enumerate(centres):
            bamboo(0, rng.uniform(-stagger, stagger), L + rng.uniform(-stagger, stagger),
                   (0, y, heights[i]), radii[i], tone=grey if i % 6 == 3 else pale)
    steps.run('bamboo deck', grid, draw_deck, note=f'{n} hollow culms, independent nodes and uneven ends')

    def draw_bearers():
        for x in stations:
            bamboo(1, -B / 2 - 0.14, B / 2 + 0.14,
                   (x, 0, deck_z - D / 2 - cross_r * 0.5), cross_r, hollow=False, tone=warm)
    steps.run('under-deck bearers', grid, draw_bearers, note='two transverse supports below the deck')

    def draw_crossbars():
        for x in stations:
            bamboo(1, -B / 2 - 0.23, B / 2 + 0.23,
                   (x, 0, cross_z), cross_r, hollow=True)
    steps.run('lashed crossbars', grid, draw_crossbars, note='two substantial bamboo crossbars above the deck')

    def draw_posts():
        if post_h > 0:
            for side in (-1, 1):
                bamboo(2, 0.015, deck_z + post_h,
                       (stations[1], side * (B / 2 + 0.09), 0), post_r,
                       lean=side * 0.025, hollow=True)
                # Short rough side pegs, as in the photo, tied across the posts.
                bamboo(0, stations[1] - 0.34, stations[1] + 0.24,
                       (0, side * (B / 2 + 0.09), cross_z + cross_r * 0.65),
                       voxel_m * 1.3, hollow=False, tone=warm)
    if post_h > 0:
        steps.run('upright posts and pegs', grid, draw_posts, note='two outboard posts at the far crossbar')
    else:
        steps.note('upright posts and pegs', 'frame_drop_m is 0')

    def vx(x, y, z):
        return _to_vox((x, y, z), lo, voxel_m)

    def cord(points, mat=rope):
        _polyline(grid, [vx(*p) for p in points], rope_r / voxel_m, mat)

    def draw_lashings():
        for x in stations:
            for i in np.linspace(0, n - 1, min(n, 7)).astype(int):
                y, z, r = centres[i], heights[i], radii[i]
                for turn in (-1, 1):
                    pts = []
                    for a in np.linspace(0, 2 * math.pi, 57):
                        sn, cs = math.sin(a), math.cos(a)
                        zz = z + (r + rope_r * 0.5) * sn
                        zz += max(0, sn) ** 3 * (cross_z + cross_r - z - r)
                        pts.append((x + turn * voxel_m * 1.5, y + (r + rope_r * 0.5) * cs, zz))
                    cord(pts)
                cord([(x - 2 * voxel_m, y, cross_z + cross_r),
                      (x + 2 * voxel_m, y + voxel_m, cross_z + cross_r),
                      (x + 0.10, y + 0.03, z + r)])
            # Rope collar on each overhanging crossbar end, with a loose tail.
            for side in (-1, 1):
                y = side * (B / 2 + 0.10)
                for offset in (-voxel_m, voxel_m):
                    cord([(x + (cross_r + rope_r * 0.4) * math.cos(a),
                           y + offset, cross_z + (cross_r + rope_r * 0.4) * math.sin(a))
                          for a in np.linspace(0, 2 * math.pi, 57)])
                cord([(x, y, cross_z + cross_r), (x + 0.09, y, cross_z + 0.03),
                      (x + 0.14, y + side * 0.03, cross_z - 0.08)])
    steps.run('rope lashings', grid, draw_lashings, note='paired rope turns, tightening knots and short tails')
    steps.rows[0]['culms'] = metrics
    steps.rows[0]['deck_culms'] = n
    return grid
