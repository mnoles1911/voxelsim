"""Coarser copies of a baked asset, for drawing it at a distance.

WHY A DOWNSAMPLE AND NOT A COARSER BUILD. `resolution_cm` will happily generate
the same species at 10 cm instead of 2 cm, and the result is a DIFFERENT-LOOKING
ANIMAL -- the generator re-solves every feature against the new lattice, so a
muzzle that was eight voxels becomes two and the silhouette moves. That is fine
for authoring and wrong for a level of detail, where the whole contract is "the
same individual, further away". A reduction of the baked grid cannot introduce
anything that was not there; it can only lose information. So LOD 0 is built and
every level below it is reduced.

WHY IT IS NEEDED AT ALL, in one number: at 1080p and a 70 degree vertical field
of view one pixel subtends 1.1312 mrad, so a 2 cm voxel is smaller than a pixel
beyond 17.7 m. Every animal in the library is authored finer than it can be seen
past arm's reach. `docs/wildlife-lod-and-rings.md` has the ladder; measured on a
white-tailed deer it runs 16,742 voxels at 2 cm to 60 at 20 cm and 5 at 50 cm.

THE RULE FOR "IS THIS BLOCK SOLID". A block of the fine grid becomes one coarse
voxel if enough of it is solid, and takes the most common material among the
solid voxels in it. `OCCUPANCY` is deliberately LOW (0.15, not 0.5): a majority
rule erodes thin parts first, and thin parts -- legs, antlers, a tail -- are
exactly what makes an animal recognisable at the range where LOD is used. A deer
that loses its legs at 200 m reads as a boulder. Erring toward solid keeps the
silhouette and costs a little bulk, which is the right trade in this direction.

MATERIALS ARE VOTED, NOT AVERAGED. Material ids are an enum, not a scale --
averaging `skin_white` and `skin_black` would produce whatever id happens to sit
between them, which on a killer whale is a different substance entirely. The
mode over the solid voxels is the only defensible choice.

WHAT THIS DOES NOT DO: parts and joints. A reduced grid drops its part tags,
because a 5-voxel deer has nowhere to put four legs, two ears and a tail and a
rig that claims otherwise would be lying to the animator. LOD grids are for
DRAWING. Anything that needs to move a limb needs LOD 0. That is consistent with
the ring design, where the far ring has no animation.
"""
from __future__ import annotations

import numpy as np

# Fraction of a block that must be solid for the coarse voxel to be solid.
# See the module docstring: low on purpose, to protect thin features.
OCCUPANCY = 0.15


def reduce_grid(data: np.ndarray, factor: int) -> np.ndarray:
    """`data` reduced by an integer `factor` on every axis.

    Vectorised rather than looped: a 3-level ladder over 688 banks is 2,064
    reductions and the naive triple loop took long enough to discourage running
    it, which is how a tool ends up never being used.
    """
    if factor < 2:
        raise ValueError(f"factor must be >= 2, got {factor}")
    f = int(factor)
    # Pad up to a whole number of blocks so the edge of the asset is not
    # silently cropped -- losing the tip of an antler to integer division is a
    # shape change nobody would see in a count.
    pad = [(0, (-s) % f) for s in data.shape]
    p = np.pad(data, pad, mode="constant", constant_values=0)
    nx, ny, nz = (s // f for s in p.shape)
    # (nx, f, ny, f, nz, f) -> (nx, ny, nz, f*f*f)
    blocks = (p.reshape(nx, f, ny, f, nz, f)
               .transpose(0, 2, 4, 1, 3, 5)
               .reshape(nx, ny, nz, f * f * f))

    solid = blocks != 0
    keep = solid.sum(axis=3) >= max(1, int(round(OCCUPANCY * f * f * f)))

    out = np.zeros((nx, ny, nz), dtype=data.dtype)
    if not keep.any():
        return out
    # Mode of the non-zero material ids, per kept block. bincount over the
    # flattened block axis, with air excluded by construction.
    idx = np.argwhere(keep)
    for i, j, k in idx:
        vals = blocks[i, j, k]
        vals = vals[vals != 0]
        counts = np.bincount(vals)
        out[i, j, k] = counts.argmax()
    return out


# The fewest voxels a reduced grid may have and still be worth writing.
#
# MEASURED, NOT CHOSEN: reducing `red-fox` (1,717 voxels at 2 cm) by 25 yields
# ZERO. The animal disappears, and what would be written is a valid VXA file
# containing no voxels -- an asset that loads, validates, composes and draws
# nothing. That is this project's signature failure with a file extension. Below
# this floor the ladder stops and the renderer should use a point or a billboard,
# which is the honest representation of something under a pixel anyway.
MIN_VOXELS = 4


def factorise(target: int) -> list:
    """`target` as a chain of small factors, coarsest reduction last.

    CASCADE, DO NOT JUMP -- and this is measured. Reducing a deer straight by 10
    keeps 43 voxels; reducing by 5 and then by 2 keeps 68. A wolf goes 23 against
    36, a fox 3 against 8. The reason is the occupancy test: one big block asks
    "is 15% of 1,000 fine voxels solid" and a leg never is, while the same
    question asked twice over smaller blocks lets a leg survive the first round
    and vote in the second. The difference is entire limbs on exactly the assets
    where the silhouette is all that is left.
    """
    out, n = [], int(target)
    for p in (5, 3, 2):
        while n % p == 0:
            out.append(p)
            n //= p
    if n > 1:                      # a prime we do not split, e.g. 7
        out.append(n)
    return sorted(out, reverse=True)


def reduce_to(data: np.ndarray, target: int) -> np.ndarray:
    """`data` reduced by `target` on every axis, cascaded through `factorise`."""
    cur = data
    for f in factorise(target):
        cur = reduce_grid(cur, f)
    return cur


def reduce_origin(origin, factor: int):
    """The anchor moves with the lattice.

    `origin` is in FINE voxels and the reduced grid is in coarse ones, so the
    anchor has to be divided too -- and it must FLOOR, matching the way
    `reduce_grid` pads at the high end and starts blocks at index 0. Rounding
    instead of flooring shifts the asset by up to half a coarse voxel against
    the terrain, which at 50 cm is a quarter of a metre of float or sink.
    """
    f = int(factor)
    return tuple(int(np.floor(v / f)) for v in origin)


def ladder(voxel_mm: int, levels=(2, 5, 10, 25)) -> list:
    """(factor, coarse_voxel_mm) for each level coarser than the base.

    `levels` are multiples of the BASE voxel, so a 2 cm asset yields 4, 10, 20
    and 50 cm. Levels that do not divide evenly are skipped rather than
    approximated: a non-integer factor is a resample, not a reduction, and this
    module only claims to do the latter.
    """
    out = []
    for f in levels:
        if f < 2:
            continue
        out.append((int(f), int(voxel_mm * f)))
    return out
