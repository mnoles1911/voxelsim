"""Rocks: boulders, slabs and cobbles.

Nothing here draws a skeleton, because a rock has none. It is built the way a
rock actually reads at voxel scale — an accretion of overlapping masses, sliced
by a few flat fracture planes, then eroded and part-buried.

Four steps, each one a thing you can see in the result:

1. **Accretion** — a handful of ellipsoids scattered near the centre and unioned.
   One lump gives a clean ovoid; six gives a knobbly weathered boulder.
2. **Faceting** — half-space cuts at random orientations. This is the difference
   between a river cobble and a freshly fractured block, and it is the single
   parameter that most changes what kind of stone it looks like.
3. **Erosion** — drop voxels with few solid neighbours. Rounds sharp spurs and
   pits the surface, so the silhouette stops looking like intersecting spheres.
4. **Burial** — cut everything below z=0. A boulder that sits ON the ground
   reads as dropped; one cut into looks settled.
"""

from __future__ import annotations

import math

import numpy as np

from . import materials
from .grid import VoxelGrid, m_to_vox
from .spec import get


def build(spec: dict, rng: np.random.Generator, voxel_m: float) -> VoxelGrid:
    size = float(get(spec, "rock.size_m"))
    lumps = int(get(spec, "rock.lumps"))
    spread = float(get(spec, "rock.spread"))
    flatten = float(get(spec, "rock.flatten"))
    elongate = float(get(spec, "rock.elongate"))
    angular = float(get(spec, "rock.angular"))
    facets = int(get(spec, "rock.facets"))
    erode = float(get(spec, "rock.erode"))
    bury = float(get(spec, "rock.bury"))
    rubble = float(get(spec, "rock.rubble"))
    mat = materials.resolve(get(spec, "materials.rock"))

    # Half-extents in metres, then voxels. The grid gets a margin for rubble
    # and for the erosion pass to work against.
    hx = size * 0.5 * elongate
    hy = size * 0.5 / max(elongate, 0.2) ** 0.5
    hz = size * 0.5 * flatten
    margin = 1.0 + rubble * 1.6

    nx = max(4, int(m_to_vox(hx * 2 * margin, voxel_m)) + 4)
    ny = max(4, int(m_to_vox(hy * 2 * margin, voxel_m)) + 4)
    nz = max(4, int(m_to_vox(hz * 2 + size * 0.2, voxel_m)) + 4)

    grid = VoxelGrid((nx, ny, nz), (0, 0, 0), voxel_m)
    cx, cy = nx / 2.0, ny / 2.0
    # Sink the body so `bury` of its height falls below the cut at z=0.
    cz = m_to_vox(hz, voxel_m) * (1.0 - 2.0 * bury) + 2.0

    # 1. accretion -----------------------------------------------------------
    for i in range(max(1, lumps)):
        if i == 0:
            off = np.zeros(3)
            scale = 1.0
        else:
            off = rng.normal(scale=spread * 0.45, size=3) * np.array([hx, hy, hz])
            scale = 0.45 + 0.55 * rng.random()
        c = np.array([cx, cy, cz]) + m_to_vox(off, voxel_m)
        _ellipsoid(grid, c,
                   m_to_vox(hx * scale, voxel_m),
                   m_to_vox(hy * scale, voxel_m),
                   m_to_vox(hz * scale, voxel_m), mat)

    # 2. faceting ------------------------------------------------------------
    if angular > 0.0 and facets > 0:
        _facet(grid, rng, facets, angular)

    # 3. erosion -------------------------------------------------------------
    if erode > 0.0:
        _erode(grid, erode, rng)

    # 4. rubble around the base ----------------------------------------------
    if rubble > 0.0:
        count = int(rubble * 14)
        for _ in range(count):
            ang = rng.random() * 2.0 * math.pi
            dist = (0.55 + 0.65 * rng.random()) * max(hx, hy)
            r = size * (0.04 + 0.10 * rng.random())
            c = np.array([cx + m_to_vox(math.cos(ang) * dist, voxel_m),
                          cy + m_to_vox(math.sin(ang) * dist, voxel_m),
                          2.0])
            rv = m_to_vox(r, voxel_m)
            _ellipsoid(grid, c, rv, rv, rv * 0.7, mat)

    return grid


def _ellipsoid(grid: VoxelGrid, c, rx: float, ry: float, rz: float, mat: int) -> None:
    rx, ry, rz = max(rx, 0.6), max(ry, 0.6), max(rz, 0.6)
    x0, x1 = int(c[0] - rx) - 1, int(c[0] + rx) + 2
    y0, y1 = int(c[1] - ry) - 1, int(c[1] + ry) + 2
    z0, z1 = int(c[2] - rz) - 1, int(c[2] + rz) + 2
    nx, ny, nz = grid.shape
    x0, y0, z0 = max(x0, 0), max(y0, 0), max(z0, 0)
    x1, y1, z1 = min(x1, nx), min(y1, ny), min(z1, nz)
    if x0 >= x1 or y0 >= y1 or z0 >= z1:
        return
    xs = (np.arange(x0, x1) + 0.5 - c[0]) / rx
    ys = (np.arange(y0, y1) + 0.5 - c[1]) / ry
    zs = (np.arange(z0, z1) + 0.5 - c[2]) / rz
    d = (xs[:, None, None] ** 2 + ys[None, :, None] ** 2 + zs[None, None, :] ** 2)
    block = grid.data[x0:x1, y0:y1, z0:z1]
    block[d <= 1.0] = mat


def _facet(grid: VoxelGrid, rng, facets: int, angular: float) -> None:
    """Slice flat faces off the mass with half-space cuts.

    The cut depth is measured ALONG EACH NORMAL, against the mass that is
    actually still there. An earlier version measured one extent for the whole
    rock -- the longest axis -- and used it for every cut, so any cut across a
    short axis landed outside the stone and removed nothing. Every rock came out
    a smooth ellipsoid and the angularity slider did nothing at all.
    """
    gx, gy, gz = np.meshgrid(np.arange(grid.shape[0]), np.arange(grid.shape[1]),
                             np.arange(grid.shape[2]), indexing="ij")
    for i in range(facets):
        occ = grid.data != 0
        total = int(occ.sum())
        if total < 8:
            return
        xs, ys, zs = np.nonzero(occ)
        centre = np.array([xs.mean(), ys.mean(), zs.mean()])

        if i == 0 and angular > 0.35:
            # A flat top, deliberately, before anything random. It is the single
            # most legible signal that a voxel lump is a ROCK rather than a
            # boulder-shaped nothing -- a horizontal face catches the light flat
            # while every side falls away, and the eye reads stone immediately.
            n = np.array([rng.normal() * 0.25, rng.normal() * 0.25, 1.0])
        else:
            n = rng.normal(size=3)
            n[2] *= 0.6           # the rest lean toward upright faces
        n = n / max(np.linalg.norm(n), 1e-6)

        d = (gx - centre[0]) * n[0] + (gy - centre[1]) * n[1] + (gz - centre[2]) * n[2]
        # Place the plane by QUANTILE of the mass, not by a fraction of the
        # reach. Depth-as-a-fraction-of-reach sounds equivalent and is not: an
        # ellipsoid's far point along a normal is much further out than its
        # bulk, so a cut at 80% of the reach shaved a cap of a few dozen voxels
        # and the silhouette stayed an egg. A quantile says what it means --
        # this plane takes this much stone off -- so `angular` reads directly as
        # how blocky the result is.
        frac = 0.05 + 0.30 * angular * (0.4 + 0.6 * rng.random())
        cut = float(np.quantile(d[occ], 1.0 - frac))
        grid.data[occ & (d > cut)] = 0


def rng_field(shape, salt: int = 0) -> np.ndarray:
    """Uniform noise the erosion pass draws against.

    Uses its own generator seeded from the grid shape rather than the build rng,
    so erosion stays deterministic without having to thread the generator
    through every call.
    """
    return np.random.default_rng(shape[0] * 73856093 ^ shape[1] * 19349663
                                 ^ shape[2] * 83492791 ^ salt).random(shape)


def _erode(grid: VoxelGrid, amount: float, rng) -> None:
    """Weather the surface by taking away coherent patches.

    Two things matter here and only one of them is obvious.

    The obvious one: erosion should bite hardest where the stone is most
    exposed. A hard neighbour-count threshold does not do that -- a flat face
    sits near 18 of 27 neighbours, so a threshold tuned to catch thin spurs
    never fires on a face and the rock stays a smooth stair-stepped ellipsoid.
    Probability rising with exposure is what actually pits a face.

    The non-obvious one: the noise has to be SPATIALLY COHERENT. Drawing an
    independent random number per voxel removes a scatter of single voxels and
    turns the surface into sponge -- and that speckle completely hid the flat
    faces the faceting pass had just cut, so an angular boulder and a river
    cobble came out looking identical. Blurring the noise first means erosion
    takes off patches a few voxels across, which reads as weathering and leaves
    the facets standing.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return
    occ = (grid.data != 0)
    if not occ.any():
        return
    neighbours = ndimage.convolve(occ.astype(np.uint8), np.ones((3, 3, 3), np.uint8),
                                  mode="constant", cval=0)
    exposure = np.clip(1.0 - neighbours / 27.0, 0.0, 1.0)

    # Patch size scales with the rock: a 5-voxel cobble cannot lose 3-voxel
    # patches and still be a cobble.
    span = max(grid.data.shape)
    sigma = max(0.8, min(3.0, span * 0.06))
    field = ndimage.gaussian_filter(rng_field(grid.data.shape, int(span)), sigma=sigma)
    lo, hi = float(field.min()), float(field.max())
    field = (field - lo) / max(hi - lo, 1e-9)

    chance = np.clip(amount * exposure ** 1.2 * 2.2, 0.0, 1.0)
    grid.data[occ & (field < chance)] = 0
