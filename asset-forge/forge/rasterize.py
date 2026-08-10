"""Skeleton -> voxels.

This is the step that makes the tool voxel-native rather than a mesh tool with
a converter bolted on. The alternative -- build a nice mesh, then voxelize it
with trimesh or cuda_voxelizer -- looks easier and is worse here: at 10 cm every
branch under a voxel across is lost or arbitrarily fattened by the sampler, and
the crown can end up not connected to the trunk. Drawing the skeleton straight
into the grid means the thin-branch decision is ours to make, once, explicitly.
See `docs/tree-asset-generator-research.md` section 4.
"""

from __future__ import annotations

import numpy as np

from . import materials
from .grid import VoxelGrid, m_to_vox
from .skeleton import Skeleton
from .spec import get


def bounds(skel: Skeleton, spec: dict, voxel_m: float) -> tuple[np.ndarray, tuple[int, int, int]]:
    """Grid origin (voxels) and shape, with room for wood and foliage."""
    pad_m = float(skel.radius.max())
    if get(spec, "foliage.enabled"):
        pad_m = max(
            pad_m,
            float(get(spec, "foliage.clump_radius_m")) + abs(float(get(spec, "foliage.droop_m"))),
        )
    pad = int(np.ceil(m_to_vox(pad_m, voxel_m))) + 2

    lo = np.floor(m_to_vox(skel.pos.min(axis=0), voxel_m)).astype(np.int64) - pad
    hi = np.ceil(m_to_vox(skel.pos.max(axis=0), voxel_m)).astype(np.int64) + pad
    lo[2] = 0  # the base sits on the ground plane; nothing below it
    shape = tuple(int(v) for v in (hi - lo + 1))
    return lo, shape


def wood(grid: VoxelGrid, skel: Skeleton, spec: dict, origin: np.ndarray) -> None:
    bark = materials.resolve(get(spec, "materials.bark"))
    core = materials.resolve(get(spec, "materials.core"))
    core_mat = core if core != bark else None

    pos_vox = m_to_vox(skel.pos, grid.voxel_m) - origin
    r_vox = m_to_vox(skel.radius, grid.voxel_m)
    parents, children = skel.segments()

    for pi, ci in zip(parents, children):
        grid.capsule(
            pos_vox[pi],
            pos_vox[ci],
            float(r_vox[pi]),
            float(r_vox[ci]),
            bark,
            core_mat=core_mat,
        )


def foliage(
    grid: VoxelGrid,
    skel: Skeleton,
    spec: dict,
    origin: np.ndarray,
    rng: np.random.Generator,
) -> int:
    """Leaf clumps on the twigs. Returns how many clumps were placed."""
    if not get(spec, "foliage.enabled"):
        return 0

    leaf = materials.resolve(get(spec, "materials.leaf"))
    min_order = int(get(spec, "foliage.min_order"))
    coverage = float(get(spec, "foliage.coverage"))
    density = float(get(spec, "foliage.density"))
    squash = float(get(spec, "foliage.squash"))
    droop = float(get(spec, "foliage.droop_m"))
    r_vox = m_to_vox(float(get(spec, "foliage.clump_radius_m")), grid.voxel_m)

    # Clumps go on every twig, not only on the branch tips. Tips alone are few,
    # which forces each clump to be large to fill the crown, and a crown built
    # from a dozen big spheres reads as a pile of balls rather than foliage.
    # Many small clumps following the twigs read as a canopy.
    twig_max = float(get(spec, "growth.tip_radius_m")) * 3.0
    cand = np.flatnonzero((skel.order >= min_order) & (skel.radius <= twig_max))
    if cand.size == 0:
        # The tree never forked deeply enough to have twigs of that order.
        # Fall back to every tip so the species still renders as foliated,
        # rather than silently producing a bare skeleton.
        cand = np.flatnonzero(skel.is_tip)
    if cand.size == 0:
        return 0

    # Thin the candidates so clump centres keep their distance BEFORE the
    # coverage roll. Picking twigs at random and hoping for gaps does not work:
    # twigs are dense, so any coverage high enough to fill the crown also packs
    # the clumps until they fuse. Enforcing a minimum separation is what gives a
    # canopy distinct masses with daylight between them.
    separation = float(get(spec, "foliage.separation"))
    min_dist = float(get(spec, "foliage.clump_radius_m")) * separation
    if min_dist > 0.0 and cand.size > 1:
        cand = cand[_thin_by_distance(skel.pos[cand], min_dist, rng)]

    if coverage < 1.0:
        cand = cand[rng.random(cand.size) < coverage]
    if cand.size == 0:
        return 0

    # Vary each clump's size and position, so the canopy does not resolve into
    # a lattice of identical spheres on close inspection.
    jitter = float(get(spec, "foliage.clump_jitter"))
    radii = r_vox * (1.0 + jitter * (rng.random(cand.size) * 2.0 - 1.0))
    offsets = rng.normal(scale=jitter * r_vox * 0.5, size=(cand.size, 3))

    placed = 0
    droop_vox = m_to_vox(droop, grid.voxel_m)
    for k, i in enumerate(cand):
        anchor = m_to_vox(skel.pos[i], grid.voxel_m) - origin
        shift = offsets[k].copy()
        shift[2] -= droop_vox
        r = max(radii[k], 1.0)
        # Keep the clump ON the twig it hangs from. Droop and jitter together
        # can carry a clump clear of its anchor, and the ball then sits in mid
        # air joined to nothing. That was invisible at 10 cm, where a clump is
        # two voxels across and any near miss still touches, and at 2 cm it left
        # whole thousand-voxel leaf masses floating -- the single largest source
        # of loose voxels in the library. Capping the displacement at three
        # quarters of the radius keeps every clump overlapping its twig while
        # leaving droop and jitter their visible range.
        reach = float(np.linalg.norm(shift))
        if reach > r * 0.75:
            shift *= (r * 0.75) / reach
        grid.blob(
            anchor + shift, r, leaf, rng, density=density, squash=squash, only_air=True
        )
        # A solid core ON the twig, whatever the thinning did.
        #
        # Capping the displacement keeps the clump's VOLUME over its anchor, but
        # the voxels that actually bridge to the wood are the innermost ones,
        # and `density` is free to remove those like any others. When it did,
        # the clump ended up a shell starting two voxels out and floating -- two
        # of them on one acacia, twelve hundred voxels each. This is a handful
        # of voxels that guarantees the join rather than leaving it to chance.
        grid.ball(anchor, min(r * 0.4, 1.7), leaf, only_air=True)
        placed += 1
    return placed


def _thin_by_distance(points: np.ndarray, min_dist: float, rng) -> np.ndarray:
    """Greedy dart-throwing: keep points no closer than `min_dist` to each other.

    Visits candidates in random order and accepts one only if nothing already
    accepted is within the radius, using a spatial hash so the check stays local
    rather than comparing against every accepted point.
    """
    cell = max(min_dist, 1e-6)
    buckets: dict[tuple[int, int, int], list[int]] = {}
    keep: list[int] = []
    r2 = min_dist * min_dist

    for i in rng.permutation(points.shape[0]):
        p = points[i]
        cx, cy, cz = (int(np.floor(p[0] / cell)), int(np.floor(p[1] / cell)),
                      int(np.floor(p[2] / cell)))
        clash = False
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for j in buckets.get((cx + dx, cy + dy, cz + dz), ()):
                        d = points[j] - p
                        if float(d @ d) < r2:
                            clash = True
                            break
                    if clash:
                        break
                if clash:
                    break
            if clash:
                break
        if not clash:
            buckets.setdefault((cx, cy, cz), []).append(int(i))
            keep.append(int(i))
    return np.asarray(sorted(keep), dtype=np.int64)


def frond_blades(
    grid: VoxelGrid,
    skel: Skeleton,
    spec: dict,
    origin: np.ndarray,
    rng: np.random.Generator,
) -> int:
    """Leaf blades along each frond's midrib.

    Not clumps on twigs: the blade is widest about a third of the way out and
    tapers to a point at both ends, and it is flattened vertically because a
    palm leaf is a plane of leaflets, not a sausage. Stamping a squashed
    ellipsoid at every midrib node draws exactly that, and at voxel resolution
    a dense row of overlapping ellipsoids *is* a blade.
    """
    if not get(spec, "foliage.enabled") or skel.along is None:
        return 0

    leaf = materials.resolve(get(spec, "materials.leaf"))
    width = float(get(spec, "frond.width_m"))
    squash = float(get(spec, "foliage.squash"))
    density = float(get(spec, "foliage.density"))
    jitter = float(get(spec, "foliage.clump_jitter"))

    cand = np.flatnonzero((skel.order >= 1) & (skel.along > 0.0))
    placed = 0
    for i in cand:
        s = float(skel.along[i])
        # Peak just inboard of centre, zero at both ends.
        profile = (4.0 * s * (1.0 - s)) ** 0.55 if 0.0 < s < 1.0 else 0.0
        w = width * profile * (1.0 + jitter * (rng.random() - 0.5) * 0.5)
        r_vox = m_to_vox(w, grid.voxel_m)
        if r_vox < 0.6:
            continue
        c = m_to_vox(skel.pos[i], grid.voxel_m) - origin
        grid.blob(c, r_vox, leaf, rng, density=density, squash=squash, only_air=True)
        placed += 1
    return placed


def ground_contact(grid: VoxelGrid) -> int:
    """How many solid voxels sit on the bottom slab.

    Zero means the asset would float when placed, which is the failure a
    stamped voxel tree shows most obviously and most embarrassingly.
    """
    return int(np.count_nonzero(grid.data[:, :, 0]))
