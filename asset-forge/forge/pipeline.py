"""spec + seed -> voxels. The one entry point everything else calls.

`build()` is deterministic: the same spec and the same seed produce byte-for-byte
the same grid, on any machine, today or in six months. That is what lets a tree
be stored as a few hundred bytes instead of a voxel blob, and it is what makes
pre-baked seed banks safe -- the world can hold `(species, seed)` and trust that
what it gets back is what the designer approved.

The seed is mixed with a hash of the spec, so editing one slider reshuffles the
whole family rather than leaving seed 7 looking suspiciously like it did before
the edit.
"""

from __future__ import annotations

import os
import time
from dataclasses import dataclass, field
from typing import Any

import numpy as np

from . import materials, rasterize
from .grid import VoxelGrid, dense_bytes
from .skeleton import Skeleton, grow
from .spec import get, realize as spec_realize, spec_hash


@dataclass
class Tree:
    grid: VoxelGrid
    skeleton: Skeleton
    spec: dict          # the species, as authored
    seed: int
    realized: dict = field(default_factory=dict)  # this individual of it
    stats: dict[str, Any] = field(default_factory=dict)

    @property
    def name(self) -> str:
        return f"{get(self.spec, 'name')}-{self.seed:04d}"


# A dense grid this big is refused rather than allowed to thrash the machine.
# A 28 m tree at 2 cm needs about 1.7 GB, so the default leaves headroom for it
# while still catching a slider combination that would ask for 40 GB.
MAX_GRID_MB = int(os.environ.get("ASSET_FORGE_MAX_GRID_MB", "3072"))


class GridTooLarge(RuntimeError):
    """Raised before allocating, with the numbers and the way out."""


def resolution_m(spec: dict, override=None) -> float:
    """Metres per voxel for this build. `override` wins, for coarse previews."""
    cm = float(override) if override else float(get(spec, "resolution_cm"))
    return cm / 100.0


def rng_for(spec: dict, seed: int) -> np.random.Generator:
    h = int(spec_hash(spec), 16)
    return np.random.default_rng([int(seed), h & 0xFFFFFFFF, (h >> 32) & 0xFFFFFFFF])


def build(spec: dict, seed: int, *, connectivity: bool = True,
          resolution_cm=None) -> Tree:
    t0 = time.perf_counter()
    voxel_m = resolution_m(spec, resolution_cm)
    rng = rng_for(spec, seed)

    # Pick this individual out of the species before growing it, so two seeds
    # differ in height, spread and lean and not only in twig placement.
    live, _ = spec_realize(spec, rng)

    skel = grow(live, rng)
    t_grow = time.perf_counter()

    origin, shape = rasterize.bounds(skel, live, voxel_m)
    need_mb = dense_bytes(shape) / 1e6
    if need_mb > MAX_GRID_MB:
        raise GridTooLarge(
            f"{get(live, 'name')} at {voxel_m * 100:g} cm needs a "
            f"{shape[0]}x{shape[1]}x{shape[2]} grid ({need_mb / 1000:.1f} GB), over the "
            f"{MAX_GRID_MB / 1000:.1f} GB limit. Use a coarser voxel size, a smaller "
            f"tree, or raise ASSET_FORGE_MAX_GRID_MB."
        )
    grid = VoxelGrid(shape, tuple(origin), voxel_m)
    rasterize.wood(grid, skel, live, origin)
    clumps = rasterize.foliage(grid, skel, live, origin, rng)
    t_raster = time.perf_counter()

    grid = grid.crop()

    stats: dict[str, Any] = {
        "seed": seed,
        "spec_hash": spec_hash(spec),
        "nodes": skel.n,
        "segments": int(skel.n - 1),
        "max_order": int(skel.order.max()) if skel.n else 0,
        "iterations": skel.iterations,
        "targets_left": skel.targets_left,
        "clumps": clumps,
        "voxel_cm": round(voxel_m * 100, 4),
        "height_m": round(grid.shape[2] * voxel_m, 2),
        "footprint_m": (
            round(grid.shape[0] * voxel_m, 2),
            round(grid.shape[1] * voxel_m, 2),
        ),
        "grid_mb": round(need_mb, 1),
        "extent_vox": tuple(int(v) for v in grid.shape),
        "voxels": grid.count(),
        "by_material": grid.histogram(),
        "ground_contact": rasterize.ground_contact(grid),
        "ms_grow": round((t_grow - t0) * 1e3, 1),
        "ms_raster": round((t_raster - t_grow) * 1e3, 1),
    }
    if connectivity:
        wood_ids = {
            materials.resolve(get(live, "materials.bark")),
            materials.resolve(get(live, "materials.core")),
        }
        wood = grid.material_mask(wood_ids)
        wood_total = int(wood.sum())
        wood_frac = grid.component_fraction(wood, connectivity=1)
        stats["wood_connected"] = round(wood_frac, 4)
        stats["wood_detached"] = int(round(wood_total * (1.0 - wood_frac)))
        attached = grid.component_fraction(None, connectivity=3)
        stats["attached_frac"] = round(attached, 4)
        stats["detached"] = int(round(stats["voxels"] * (1.0 - attached)))
    stats["ms_total"] = round((time.perf_counter() - t0) * 1e3, 1)

    return Tree(grid=grid, skeleton=skel, spec=spec, seed=seed, realized=live, stats=stats)


def health(tree: Tree) -> list[str]:
    """Things that would make this asset unusable in the world.

    Reported per tree rather than asserted, because a spec being explored with
    sliders is allowed to be briefly nonsense; a spec being saved to the
    library is not.
    """
    problems: list[str] = []
    s = tree.stats

    if s["voxels"] == 0:
        problems.append("empty: no voxels generated")
        return problems
    if s.get("wood_detached", 0):
        problems.append(
            f"broken: {s['wood_detached']} wood voxels are not joined to the trunk"
        )
    if s.get("detached", 0) > max(8, 0.01 * s["voxels"]):
        problems.append(f"loose: {s['detached']:,} voxels float free of the tree")
    if s["ground_contact"] == 0:
        problems.append("floating: nothing touches the ground plane")
    if s["max_order"] == 0:
        problems.append("bare: the trunk never branched")
    if max(s["extent_vox"]) > 256:
        problems.append(
            f"large: {max(s['extent_vox'])} voxels on the long axis, over the 256 "
            "limit for a single .vox model (export will split it)"
        )
    if get(tree.spec, "foliage.enabled") and s["clumps"] == 0:
        problems.append("bald: foliage is on but no clump was placed")
    return problems
