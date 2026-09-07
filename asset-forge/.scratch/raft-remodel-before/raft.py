"""Field-built rough timber raft. Visual references: docs/raft-2026-09-05.md.

Shape and surface share log-local coordinates: bark loss follows raised plates,
branch collars disturb the outline, and cut faces expose the wood underneath.
No texture, rotated raster, independent voxel deletion or new material IDs.
"""
from __future__ import annotations

import math
import numpy as np

from . import materials
from .grid import VoxelGrid
from .spec import get


def _noise(x, y, z, salt):
    """Smooth value field at arbitrary coordinates, with no angular seam."""
    x, y, z = np.broadcast_arrays(x, y, z)
    axes = [np.floor(a).astype(np.int64) for a in (x, y, z)]
    weights = [a - b for a, b in zip((x, y, z), axes)]
    weights = [w * w * (3 - 2 * w) for w in weights]
    value = np.zeros(x.shape, dtype=np.float64)
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                h = ((axes[0] + dx) * 73856093
                     ^ (axes[1] + dy) * 19349663
                     ^ (axes[2] + dz) * 83492791 ^ salt) & 0xffffffff
                h = ((h ^ (h >> 13)) * 1274126177) & 0xffffffff
                h ^= h >> 16
                w = ((weights[0] if dx else 1 - weights[0])
                     * (weights[1] if dy else 1 - weights[1])
                     * (weights[2] if dz else 1 - weights[2]))
                value += (h / 4294967295.0) * w
    return value


def build(spec, rng, voxel_m, steps):
    from .artifact import _axes, _paint, _polyline, _shape_for, _to_vox

    L = float(get(spec, "artifact.length_m"))
    B = float(get(spec, "artifact.beam_m"))
    D = float(get(spec, "artifact.depth_m"))
    n_poles = int(get(spec, "artifact.thwarts"))
    pole_r = max(voxel_m, float(get(spec, "artifact.thwart_t_m")) / 2)
    station = float(get(spec, "artifact.deck_frac"))
    age = float(get(spec, "artifact.strake_share"))
    stagger = float(get(spec, "artifact.asym_vox")) * voxel_m
    wood = materials.resolve(get(spec, "materials.frame"))
    bark = materials.resolve(get(spec, "materials.hull"))
    weathered = materials.resolve(get(spec, "materials.strake"))
    rope = materials.resolve(get(spec, "materials.trim"))
    # Existing engine colour slots, as for the canoe/glider's buff and horn.
    # Their similar face colours avoid upright-tree end-grain striping.
    dark = materials.MAT_BEAK_HORN
    pale = materials.MAT_PLUME_BUFF

    count = max(3, int(round(B / D)))
    radii = D / 2 * rng.uniform(0.76, 1.24, count)
    ys = np.zeros(count)
    for i in range(1, count):
        ys[i] = ys[i - 1] + 0.85 * (radii[i - 1] + radii[i])
    scale = B / (ys[-1] + radii[0] + radii[-1])
    radii *= scale
    ys *= scale
    ys -= (ys[-1] + radii[-1] - radii[0]) / 2
    # Per-log histories: mostly debarked, bark-rich, and older silvered wood.
    histories = rng.permutation(np.linspace(0.23, 0.77, count))
    logs = []
    for i in range(count):
        logs.append(dict(y=ys[i], r=radii[i],
                         z=D - radii[i] + rng.uniform(-0.01, 0.01),
                         taper=rng.uniform(0.065, 0.14) * (-1 if i % 2 else 1),
                         bow=rng.uniform(-0.025, 0.025),
                         oval=rng.uniform(0.92, 1.06),
                         phase=rng.uniform(0, 6.28),
                         x0=rng.uniform(-stagger, stagger),
                         x1=L + rng.uniform(-stagger, stagger),
                         tilt=rng.uniform(-0.16, 0.16),
                         salt=int(rng.integers(1, 100000000)),
                         history=histories[i],
                         knots=[(rng.uniform(0.25, L - 0.25),
                                 rng.uniform(0.1, math.pi - 0.1),
                                 rng.uniform(0.055, 0.10))
                                for _ in range(int(rng.integers(3, 7)))]))

    pad = 5 * voxel_m
    lo = np.array([-stagger - pad, -B / 2 - pad, -D * 0.7 - pad])
    hi = np.array([L + stagger + pad, B / 2 + pad, D + pole_r * 3 + pad])
    shape = _shape_for(lo, hi, voxel_m)
    grid = VoxelGrid(shape, (0, 0, 0), voxel_m)
    X, Y, Z = _axes(lo, shape, voxel_m)
    stations = ([] if n_poles == 0 else [L / 2] if n_poles == 1 else
                np.linspace(L * station, L * (1 - station), n_poles))

    def section(log, x):
        f = x / L
        cy = log['y'] + log['bow'] * np.sin(math.pi * f)
        cz = log['z'] + 0.012 * np.sin(2 * math.pi * f + log['phase'])
        r = log['r'] * (1 + log['taper'] * (2 * f - 1))
        return cy, cz, r

    metrics = []
    def draw_logs():
        for log in logs:
            # Evaluate only the strip this log can reach; do not run every
            # surface field through the other six logs and the surrounding air.
            reach = log['r'] * 1.3 + abs(log['bow']) + 3 * voxel_m
            j0 = max(0, int((log['y'] - reach - lo[1]) / voxel_m))
            j1 = min(shape[1], int((log['y'] + reach - lo[1]) / voxel_m) + 2)
            local_y = Y[:, j0:j1, :]
            local_grid = VoxelGrid((1, 1, 1), voxel_m=voxel_m)
            local_grid.data = grid.data[:, j0:j1, :]
            # Hold the section steady through the short chopped end zone.
            # Otherwise a receding bark flake can leave a corner-only tip at
            # the oblique cut. The cut still has its own uneven depth field.
            xg = np.clip(X, log['x0'] + 0.12, log['x1'] - 0.12)
            cy, cz, r = section(log, xg)
            dy, dz = local_y - cy, (Z - cz) / log['oval']
            theta = np.arctan2(dz, dy)
            u, v = np.cos(theta), np.sin(theta)
            radial = np.sqrt(dy * dy + dz * dz)
            salt = log['salt']
            # Long but finite patches, broken by a second scale. The field is
            # sampled on a cylinder in 3D, avoiding a stripe at theta=+-pi.
            patches = _noise(xg / 0.48, u * 2.6, v * 2.6, salt)
            flakes = _noise(xg / 0.14, u * 6, v * 6, salt + 31)
            boundary = 0.73 * patches + 0.27 * flakes
            bark_on = boundary > log['history']
            rough = (flakes - 0.5) * voxel_m * 1.1
            radius = r + rough + bark_on * voxel_m * 0.42
            knot_masks = []
            for kx, angle, kr in log['knots']:
                arc = np.arctan2(np.sin(theta - angle), np.cos(theta - angle)) * r
                q = ((xg - kx) / (kr * 1.7)) ** 2 + (arc / kr) ** 2
                # Rounded branch collar with a shallow chopped-off stub.
                radius = radius + 0.038 * np.exp(-q * 1.8)
                knot_masks.append(q)
            # Each end has a different oblique chop and splinter silhouette.
            chips = (_noise(0, u * 4, v * 4, salt + 127) - 0.5) * voxel_m * 2
            start = log['x0'] + log['tilt'] * dy + chips
            end = log['x1'] - log['tilt'] * dz - chips * 1.6
            # Local end checks open into the cut face and stop within the log.
            end_depth = np.minimum(X - start, end - X)
            crack_angle = log['phase'] + 0.11 * np.sin(xg * 4)
            crack = ((np.abs(np.sin(theta - crack_angle)) * radial < voxel_m * 0.30)
                     & (radial > r * 0.44) & (end_depth < 0.10)
                     & (np.cos(theta - crack_angle) > 0))
            mask = ((radial <= radius) & (X >= start) & (X <= end) & ~crack)
            surface = radial > radius - voxel_m * 1.3
            ends = end_depth <= voxel_m * 1.1
            # Solid interior, muted warm wood with grey exposed weathered areas.
            _paint(local_grid, mask, wood)
            silver = patches + 0.16 * v > 0.69 - 0.35 * age
            _paint(local_grid, mask & surface & silver, weathered)
            _paint(local_grid, mask & surface & ~bark_on & (patches < 0.39), pale)
            _paint(local_grid, mask & surface & bark_on, bark)
            _paint(local_grid, mask & surface & bark_on & (flakes < 0.38), dark)
            # Fresh scrapes at the ragged boundary, not outlines round every patch.
            peel_edge = (np.abs(boundary - log['history']) < 0.035) & (flakes > 0.67)
            _paint(local_grid, mask & surface & peel_edge, wood)
            for q in knot_masks:
                _paint(local_grid, mask & surface & (q < 1.0), wood)
                _paint(local_grid, mask & surface & (q > 0.52) & (q < 0.92), dark)
                _paint(local_grid, mask & surface & (q < 0.23), bark)
            # Off-centre pith, pale sapwood rim, subdued irregular heartwood.
            pith_r = np.sqrt((dy - r * 0.11) ** 2 + (dz + r * 0.08) ** 2)
            _paint(local_grid, mask & ends, pale)
            _paint(local_grid, mask & ends & (pith_r < r * 0.72), wood)
            ring = np.abs(pith_r - r * (0.44 + 0.035 * np.sin(3 * theta))) < voxel_m * 0.20
            _paint(local_grid, mask & ends & ring, weathered)
            _paint(local_grid, mask & ends & (radial > radius - voxel_m * 0.7) & bark_on, bark)
            metrics.append(dict(radius_m=log['r'], taper=log['taper'],
                                bow_m=log['bow'], knots=len(log['knots']),
                                bark_fraction=float(np.mean(bark_on[mask & surface]))))
    steps.run("rough timber", grid, draw_logs,
              note=f"{count} independently tapered, bowed logs; ragged bark islands, branch collars, split ends")

    # Crosspieces follow gently wandering centre fields, seated into the timber.
    pole_z = D + 0.015
    def pole_x(xk, y):
        return xk + 0.018 * np.sin(y * 2.8 + xk)

    if len(stations):
        def draw_poles():
            for k, xk in enumerate(stations):
                xp = pole_x(xk, Y)
                rr = pole_r * (1 + 0.14 * Y / B)
                tube = ((X - xp) ** 2 + (Z - pole_z) ** 2 <= rr ** 2) & (np.abs(Y) <= B / 2 + voxel_m)
                _paint(grid, tube, wood)
                patch = _noise(X * 8, Y * 4, Z * 8, 711 + k)
                _paint(grid, tube & (patch > 0.59), bark)
                _paint(grid, tube & (patch < 0.30), pale)
        steps.run("cross-poles", grid, draw_poles, note="two irregular partly peeled poles" if n_poles == 2 else f"{n_poles} poles")

        def vx(x, y, z):
            return _to_vox((x, y, z), lo, voxel_m)

        def draw_rope():
            rope_r = 0.65 * voxel_m
            for xk in stations:
                for log in logs:
                    cy, cz, r = section(log, xk)
                    xp = float(pole_x(xk, cy))
                    for turn in (-1, 0, 1):
                        pts = []
                        for angle in np.linspace(0, 2 * math.pi, 65):
                            sn, cs = math.sin(angle), math.cos(angle)
                            y = cy + (r + rope_r * 0.5) * cs
                            z = cz + (r * log['oval'] + rope_r * 0.5) * sn
                            z += max(0, sn) ** 4 * (pole_z + pole_r - cz - r * log['oval'])
                            pts.append(vx(xp + turn * 2 * voxel_m + 0.3 * voxel_m * cs, y, z))
                        _polyline(grid, pts, rope_r / voxel_m, rope)
                    z = pole_z + pole_r + 0.3 * voxel_m
                    pts = [vx(xp - 2.5 * voxel_m, cy, z),
                           vx(xp + 2.5 * voxel_m, cy + voxel_m, z),
                           vx(xp + 4 * voxel_m, cy + 2 * voxel_m, D)]
                    _polyline(grid, pts, rope_r / voxel_m, rope)
        steps.run("rope turns and knots", grid, draw_rope,
                  note=f"{len(stations) * count} crossings, three turns and a short tied tail")
    else:
        steps.note("cross-poles", "thwarts is 0")
        steps.note("rope turns and knots", "no cross-poles")
    # Non-geometric evidence for the probe, without adding spec/hash parameters.
    steps.rows[0]['logs'] = metrics
    return grid
