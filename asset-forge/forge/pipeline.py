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

from . import ground as groundlib, materials, rasterize, rock as rocklib
from .grid import VoxelGrid, dense_bytes
from .skeleton import Skeleton, add_roots, add_strands, grow, grow_frond, grow_whorl
from .spec import get, realize as spec_realize, spec_hash

# Which generator a kind goes to. Everything not listed here grows a skeleton.
BOULDER_KINDS = frozenset({"rock"})
TUFT_KINDS = frozenset({"grass", "reed", "flower"})
# Kinds with no branch structure, so the branch-shaped stats and the checks
# that read them do not apply.
BRANCHLESS = BOULDER_KINDS | TUFT_KINDS


@dataclass
class Asset:
    grid: VoxelGrid
    # None for kinds that have no branch structure at all, such as rocks.
    skeleton: Skeleton | None
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

    kind = get(live, "kind")
    model = get(live, "growth.model")
    skel = None
    clumps = 0

    if kind in BOULDER_KINDS or kind in TUFT_KINDS:
        # No skeleton. A rock is accreted and carved rather than grown; a blade
        # of grass does not branch, so there is nothing for space colonization
        # to do. Both size their own grid from what they actually traced.
        t_grow = time.perf_counter()
        gen = rocklib if kind in BOULDER_KINDS else groundlib
        grid = gen.build(live, rng, voxel_m)
        need_mb = dense_bytes(grid.shape) / 1e6
        t_raster = time.perf_counter()
    else:
        skel = {"whorl": grow_whorl, "frond": grow_frond}.get(model, grow)(live, rng)
        if model != "frond":
            skel = add_strands(skel, live, rng)
        skel = add_roots(skel, live, rng)
        t_grow = time.perf_counter()

        origin, shape = rasterize.bounds(skel, live, voxel_m)
        need_mb = dense_bytes(shape) / 1e6
        if need_mb > MAX_GRID_MB:
            raise GridTooLarge(
                f"{get(live, 'name')} at {voxel_m * 100:g} cm needs a "
                f"{shape[0]}x{shape[1]}x{shape[2]} grid ({need_mb / 1000:.1f} GB), over the "
                f"{MAX_GRID_MB / 1000:.1f} GB limit. Use a coarser voxel size, a smaller "
                f"asset, or raise ASSET_FORGE_MAX_GRID_MB."
            )
        grid = VoxelGrid(shape, tuple(origin), voxel_m)
        rasterize.wood(grid, skel, live, origin)
        clumps = (rasterize.frond_blades if model == "frond" else rasterize.foliage)(
            grid, skel, live, origin, rng)
        t_raster = time.perf_counter()

    grid = grid.crop()

    stats: dict[str, Any] = {
        "seed": seed,
        "spec_hash": spec_hash(spec),
        "kind": kind,
        "nodes": skel.n if skel else 0,
        "segments": int(skel.n - 1) if skel else 0,
        "max_order": int(skel.order.max()) if skel and skel.n else 0,
        "iterations": skel.iterations if skel else 0,
        "targets_left": skel.targets_left if skel else 0,
        "clumps": clumps,
        "voxel_cm": round(voxel_m * 100, 4),
        "height_m": round(grid.shape[2] * voxel_m, 2),
        "footprint_m": (
            round(grid.shape[0] * voxel_m, 2),
            round(grid.shape[1] * voxel_m, 2),
        ),
        "grid_mb": round(need_mb, 1),
        "extent_vox": tuple(int(v) for v in grid.shape),
        # How many .vox models this needs: the format caps a model at 256
        # voxels per axis, so anything larger is written as a scene of several.
        "vox_models_needed": int(np.prod([
            max(1, -(-int(v) // 256)) for v in grid.shape])),
        "voxels": grid.count(),
        "by_material": grid.histogram(),
        "ground_contact": rasterize.ground_contact(grid),
        "ms_grow": round((t_grow - t0) * 1e3, 1),
        "ms_raster": round((t_raster - t_grow) * 1e3, 1),
    }
    if connectivity and kind in TUFT_KINDS:
        # Ground cover has no wood, but "is this one piece?" is exactly the
        # question that matters for it -- a tuft whose blades do not reach the
        # root crown is a handful of floating threads.
        attached = grid.component_fraction(None, connectivity=3)
        stats["attached_frac"] = round(attached, 4)
        stats["detached"] = int(round(stats["voxels"] * (1.0 - attached)))
    elif connectivity and kind not in BRANCHLESS:
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

    return Asset(grid=grid, skeleton=skel, spec=spec, seed=seed, realized=live, stats=stats)


# Kept as an alias: the tool grew up as a tree generator and plenty of call
# sites still say Tree.
Tree = Asset


def health(tree: Asset) -> list[str]:
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
    kind = s.get("kind", "tree")
    if s.get("detached", 0) > max(8, 0.01 * s["voxels"]):
        problems.append(f"loose: {s['detached']:,} voxels float free of the asset")
    if s["ground_contact"] == 0:
        problems.append("floating: nothing touches the ground plane")
    if kind not in BRANCHLESS and s["max_order"] == 0:
        problems.append("bare: the trunk never branched")
    # Exceeding 256 voxels on an axis is NOT a problem and is no longer flagged.
    # It used to be rare enough to be worth pointing at; at the 5 cm asset
    # lattice most trees exceed it, the writer splits them and the selftest
    # checks the round trip. Leaving it in `health` meant ten of forty-two
    # species carried a permanent warning flag, which made "Keep all clean" skip
    # every large tree -- a check that fires on the normal case trains you to
    # ignore checks. The model count is in the stats instead.
    if (kind not in BRANCHLESS
            and get(tree.spec, "foliage.enabled") and s["clumps"] == 0):
        problems.append("bald: foliage is on but no clump was placed")
    return problems
