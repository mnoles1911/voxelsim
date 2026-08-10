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
from .grid import VOXEL_M, VoxelGrid, m_to_vox
from .skeleton import Skeleton
from .spec import get


def bounds(skel: Skeleton, spec: dict) -> tuple[np.ndarray, tuple[int, int, int]]:
    """Grid origin (voxels) and shape, with room for wood and foliage."""
    pad_m = float(skel.radius.max())
    if get(spec, "foliage.enabled"):
        pad_m = max(
            pad_m,
            float(get(spec, "foliage.clump_radius_m")) + abs(float(get(spec, "foliage.droop_m"))),
        )
    pad = int(np.ceil(m_to_vox(pad_m))) + 2

    lo = np.floor(m_to_vox(skel.pos.min(axis=0))).astype(np.int64) - pad
    hi = np.ceil(m_to_vox(skel.pos.max(axis=0))).astype(np.int64) + pad
    lo[2] = 0  # the base sits on the ground plane; nothing below it
    shape = tuple(int(v) for v in (hi - lo + 1))
    return lo, shape


def wood(grid: VoxelGrid, skel: Skeleton, spec: dict, origin: np.ndarray) -> None:
    bark = materials.resolve(get(spec, "materials.bark"))
    core = materials.resolve(get(spec, "materials.core"))
    core_mat = core if core != bark else None

    pos_vox = m_to_vox(skel.pos) - origin
    r_vox = m_to_vox(skel.radius)
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
    r_vox = m_to_vox(float(get(spec, "foliage.clump_radius_m")))

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
    for k, i in enumerate(cand):
        c = m_to_vox(skel.pos[i]) - origin + offsets[k]
        c[2] -= m_to_vox(droop)
        grid.blob(
            c, max(radii[k], 1.0), leaf, rng, density=density, squash=squash, only_air=True
        )
        placed += 1
    return placed


def ground_contact(grid: VoxelGrid) -> int:
    """How many solid voxels sit on the bottom slab.

    Zero means the asset would float when placed, which is the failure a
    stamped voxel tree shows most obviously and most embarrassingly.
    """
    return int(np.count_nonzero(grid.data[:, :, 0]))
