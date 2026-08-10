"""Rocks: boulders, slabs and cobbles.

Nothing here draws a skeleton, because a rock has none. It is built the way a
rock actually reads at voxel scale — a lumpy mass with a roughened surface,
sliced by a few flat fracture planes, then part-buried.

Four steps, each one a thing you can see in the result:

1. **Mass** — a handful of ellipsoids scattered near the centre and unioned as a
   FIELD, then the surface is pushed in and out by low-frequency noise before it
   is thresholded into voxels. The noise is not a detail pass; it is the step
   that makes the thing read as stone. Unioning ellipsoids straight into voxels
   gave a perfectly smooth surface, and a smooth curved surface voxelized at
   twenty-odd voxels across shows its stair-steps as clean concentric contour
   rings — the exact look of a Minecraft sphere, and nothing like a rock.
   Breaking the surface with noise breaks the rings, and no amount of cutting or
   eroding afterwards does that.
2. **Faceting** — half-space cuts at random orientations. This is the difference
   between a river cobble and a freshly fractured block, and it is the single
   parameter that most changes what kind of stone it looks like.
3. **Erosion** — take coherent patches off the most exposed places. With the
   noise doing the heavy lifting this is now a light finishing pass.
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
    """Build a stone whose longest dimension is `rock.size_m`.

    Faceting, erosion and the burial cut all take mass away, so the raw lumps
    have to start larger than the answer. Predicting by how much from the
    parameters did not work: a formula tuned so a boulder came out right left a
    3.2 m standing stone at 4.9 m, because the burial cut removes a different
    share of a tall stone than of a flat one. So this MEASURES instead --
    build, compare, correct, at most three times. Same seed each attempt, so the
    corrections change the scale and nothing else about the stone.
    """
    target = float(get(spec, "rock.size_m"))
    seed = int(rng.integers(1 << 62))

    # Find the scale on a COARSE copy first. The correction is a ratio of
    # lengths and barely depends on the lattice, so searching for it at the
    # authored size means paying for a 9 m boulder three times over. Coarse
    # search plus one real build is the same answer for a third of the work.
    probe_m = max(voxel_m, 0.10)
    scale = 1.0
    if probe_m > voxel_m * 1.01:
        for _ in range(3):
            probe = _build_once(spec, np.random.default_rng(seed), probe_m, scale)
            got = max(_extent_m(probe))
            if got <= 0.0:
                break
            err = target / got
            if 0.95 <= err <= 1.05:
                break
            scale = min(4.0, max(0.3, scale * err))

    grid = None
    for _ in range(3 if probe_m <= voxel_m * 1.01 else 2):
        grid = _build_once(spec, np.random.default_rng(seed), voxel_m, scale)
        got = max(_extent_m(grid))
        if got <= 0.0:
            break
        err = target / got
        if 0.92 <= err <= 1.08:
            break
        scale = min(4.0, max(0.3, scale * err))
    return grid


def _extent_m(grid: VoxelGrid) -> tuple[float, float, float]:
    occ = grid.data != 0
    if not occ.any():
        return (0.0, 0.0, 0.0)
    xs, ys, zs = np.nonzero(occ)
    return tuple(float(np.ptp(a) + 1) * grid.voxel_m for a in (xs, ys, zs))


def _build_once(spec: dict, rng: np.random.Generator, voxel_m: float,
                scale_hint: float) -> VoxelGrid:
    size = float(get(spec, "rock.size_m")) * scale_hint
    lumps = int(get(spec, "rock.lumps"))
    spread = float(get(spec, "rock.spread"))
    flatten = float(get(spec, "rock.flatten"))
    elongate = float(get(spec, "rock.elongate"))
    angular = float(get(spec, "rock.angular"))
    facets = int(get(spec, "rock.facets"))
    erode = float(get(spec, "rock.erode"))
    rough = float(get(spec, "rock.rough"))
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

    # 1. mass ----------------------------------------------------------------
    # Union the lumps as a field rather than as voxels, so the surface can be
    # displaced before it is thresholded. Values are in units of "fraction of
    # the local radius": 0 is the surface, positive is inside.
    gx, gy, gz = np.meshgrid(np.arange(nx, dtype=np.float32) + 0.5,
                             np.arange(ny, dtype=np.float32) + 0.5,
                             np.arange(nz, dtype=np.float32) + 0.5, indexing="ij")
    field = np.full((nx, ny, nz), -9.0, np.float32)
    for i in range(max(1, lumps)):
        if i == 0:
            off = np.zeros(3)
            scale = 1.0
        else:
            off = rng.normal(scale=spread * 0.45, size=3) * np.array([hx, hy, hz])
            scale = 0.45 + 0.55 * rng.random()
        c = np.array([cx, cy, cz]) + m_to_vox(off, voxel_m)
        rx = max(m_to_vox(hx * scale, voxel_m), 0.6)
        ry = max(m_to_vox(hy * scale, voxel_m), 0.6)
        rz = max(m_to_vox(hz * scale, voxel_m), 0.6)
        q = (((gx - c[0]) / rx) ** 2 + ((gy - c[1]) / ry) ** 2
             + ((gz - c[2]) / rz) ** 2)
        np.maximum(field, 1.0 - np.sqrt(q), out=field)

    relief = _surface_noise((nx, ny, nz), 1.0, int(rng.integers(1 << 30)))
    if rough > 0.0:
        field += relief * rough

    grid.data[field > 0.0] = mat

    # 2. faceting ------------------------------------------------------------
    if angular > 0.0 and facets > 0:
        # A fracture face is not a plane. Cutting with a true half-space gave
        # perfectly flat faces, and on a large rock those faces are metres
        # across -- the first 5-9 m boulders came out reading as cut gemstones
        # rather than stone. Wobbling the cut turns each face into a broken
        # surface.
        #
        # The SAME field does both jobs. Generating a second one doubled the
        # slowest part of the build for a difference nobody could see, and a
        # fracture following the same grain as the weathering is if anything the
        # more physical of the two.
        _facet(grid, rng, facets, angular,
               relief * (max(1.5, min(nx, ny, nz) * 0.10) / 0.55))

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


_SNAP_COS = 0.985   # about 10 degrees


def _snap_axis(n: np.ndarray) -> np.ndarray:
    """Pull a nearly-axis-aligned normal onto the axis.

    A cut plane a few degrees off an axis is the worst case for a voxel grid: it
    lands as a long shallow staircase of parallel ridges rather than a face.
    That artifact was reading as machining marks across the top of every rock,
    and it survived every attempt to fix it by changing the noise, because the
    noise was never what caused it. On the axis the same cut is one clean plane.
    """
    for a in range(3):
        if abs(n[a]) >= _SNAP_COS:
            out = np.zeros(3)
            out[a] = math.copysign(1.0, n[a])
            return out
    return n


def _surface_noise(shape, rough: float, salt: int) -> np.ndarray:
    """Surface displacement, in units of the local radius.

    Three octaves: two sized to the rock, which set the silhouette, and one
    sized to the voxel, which breaks up the stair-stepping. See the comment on
    the octave table below for why the third is not optional.
    """
    try:
        from scipy import ndimage
    except ImportError:
        return np.zeros(shape, np.float32)

    span = max(shape)
    out = np.zeros(shape, np.float32)

    # Two octaves scaled to the ROCK for silhouette, and one scaled to the
    # VOXEL for surface break-up.
    #
    # The rock-relative pair alone is size-dependent in a way that only shows up
    # on large stones: their wavelength is a fixed fraction of the body, so a
    # 9 m boulder gets the same six undulations a 2 m one does, and between
    # those six the surface is still a smooth curve showing clean concentric
    # stair-steps. The whole point of this pass is to stop that happening, and
    # on the first large boulders it did not, because the relief had grown with
    # the rock while the terracing had not. A third octave a couple of voxels
    # wide fixes it at every size, since terracing is a property of the lattice.
    octaves = [(max(0.9, span * 0.16), 0.62), (max(0.9, span * 0.075), 0.28),
               (1.7, 0.22)]
    for k, (sigma, weight) in enumerate(octaves):
        # Build the coarse octaves SMALL and stretch them, rather than blurring
        # at full size. Their features are a sixth of the rock wide by
        # construction, so nothing in the full-resolution field survives the
        # blur anyway -- and blurring ten million cells with a sigma of thirty
        # is most of a minute. A 9 m tor went from 41 seconds to nine on this.
        # The voxel-scale octave has a small sigma already and stays full size.
        step = max(1, int(sigma / 3.0))
        small = tuple(max(4, s // step) for s in shape)
        n = ndimage.gaussian_filter(
            rng_field(small, salt ^ (k * 7919)).astype(np.float32),
            sigma=sigma / step)
        # Blurring collapses the range, so rescale each octave to [-1, 1] rather
        # than assuming it kept one.
        lo, hi = float(n.min()), float(n.max())
        n = (n - lo) / max(hi - lo, 1e-9) * 2.0 - 1.0
        if small != shape:
            n = ndimage.zoom(n, [shape[i] / small[i] for i in range(3)], order=1)
            # zoom lands within a voxel of the target; trim or edge-extend so the
            # field is exactly grid-shaped.
            n = _fit(n, shape)
        out += n * weight
    return out * (rough * 0.55)


def _occupied_box(occ: np.ndarray) -> tuple[slice, slice, slice]:
    """Bounding box of the solid voxels, as slices."""
    idx = [np.flatnonzero(occ.any(axis=tuple(a for a in range(3) if a != ax)))
           for ax in range(3)]
    return tuple(slice(int(i[0]), int(i[-1]) + 1) if i.size else slice(0, 0)
                 for i in idx)


def _fit(a: np.ndarray, shape) -> np.ndarray:
    """Crop or edge-replicate `a` to exactly `shape`."""
    if a.shape == tuple(shape):
        return a
    a = a[:shape[0], :shape[1], :shape[2]]
    pad = [(0, max(0, shape[i] - a.shape[i])) for i in range(3)]
    return np.pad(a, pad, mode="edge") if any(p[1] for p in pad) else a


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


def _facet(grid: VoxelGrid, rng, facets: int, angular: float,
           wobble: np.ndarray | None = None) -> None:
    """Slice flat faces off the mass with half-space cuts.

    The cut depth is measured ALONG EACH NORMAL, against the mass that is
    actually still there. An earlier version measured one extent for the whole
    rock -- the longest axis -- and used it for every cut, so any cut across a
    short axis landed outside the stone and removed nothing. Every rock came out
    a smooth ellipsoid and the angularity slider did nothing at all.
    """
    # Work inside the solid's bounding box. Everything outside it is air that
    # cannot be cut, and on a large rock the grid carries a margin for rubble
    # and erosion that would otherwise be re-evaluated once per facet.
    box = _occupied_box(grid.data != 0)
    sub = grid.data[box]
    wob = wobble[box] if wobble is not None else None
    gx, gy, gz = np.meshgrid(
        np.arange(box[0].start, box[0].stop, dtype=np.float32),
        np.arange(box[1].start, box[1].stop, dtype=np.float32),
        np.arange(box[2].start, box[2].stop, dtype=np.float32), indexing="ij")

    for i in range(facets):
        occ = sub != 0
        total = int(occ.sum())
        if total < 8:
            return
        xs, ys, zs = np.nonzero(occ)
        centre = np.array([xs.mean() + box[0].start, ys.mean() + box[1].start,
                           zs.mean() + box[2].start])

        if i == 0 and angular > 0.35:
            # A flat top, deliberately, before anything random. It is the single
            # most legible signal that a voxel lump is a ROCK rather than a
            # boulder-shaped nothing -- a horizontal face catches the light flat
            # while every side falls away, and the eye reads stone immediately.
            n = np.array([0.0, 0.0, 1.0])
        else:
            n = rng.normal(size=3)
            n[2] *= 0.6           # the rest lean toward upright faces
            n = _snap_axis(n / max(np.linalg.norm(n), 1e-6))
        n = n / max(np.linalg.norm(n), 1e-6)

        d = (gx - centre[0]) * n[0] + (gy - centre[1]) * n[1] + (gz - centre[2]) * n[2]
        # Place the plane by QUANTILE of the mass, not by a fraction of the
        # reach. Depth-as-a-fraction-of-reach sounds equivalent and is not: an
        # ellipsoid's far point along a normal is much further out than its
        # bulk, so a cut at 80% of the reach shaved a cap of a few dozen voxels
        # and the silhouette stayed an egg. A quantile says what it means --
        # this plane takes this much stone off -- so `angular` reads directly as
        # how blocky the result is.
        frac = 0.04 + 0.16 * angular * (0.4 + 0.6 * rng.random())
        if i == 0 and angular > 0.35:
            # A shallow top. At full depth this one plane, combined with the
            # burial cut at z=0, turned every rock into a slanted wedge -- two
            # near-parallel faces and nothing left between them.
            frac *= 0.5
        # Quantile on a SAMPLE. Sorting a million projections eight times over
        # is most of a second for a threshold that only has to be right to a
        # fraction of a percent.
        proj = d[occ]
        if proj.size > 120_000:
            proj = proj[::proj.size // 120_000]
        cut = float(np.quantile(proj, 1.0 - frac))
        # Wobble the plane so the face comes out broken rather than machined.
        # The quantile is taken on the true plane, so the depth still means what
        # it says; only the surface it leaves behind is roughened.
        sub[occ & (d > cut + (wob if wob is not None else 0.0))] = 0


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

    # Deliberately gentle. A stronger curve ate the flat faces the faceting pass
    # had just cut and left vertical spikes along the rim, which reads as damage
    # rather than weathering.
    chance = np.clip(amount * exposure ** 1.2 * 1.1, 0.0, 0.85)
    grid.data[occ & (field < chance)] = 0
