"""VXA -- the compact native asset format.

What actually ships to the engine. Deliberately trivial to read from C++: a
fixed header, then run-length pairs of (material, count) over the grid in
x-major order. No palette (material IDs are the engine's own), no compression
beyond the runs, no scene graph.

A tree is mostly air inside its bounding box, so runs do the heavy lifting: a
typical broadleaf lands around 3-5% of its dense size.

    magic     "VXA1"               4 bytes  (unchanged across versions)
    version   uint32               = 2
    origin    int32 x, y, z        voxel offset from the asset's base, which is
                                   at (0, 0, 0) with z up
    size      uint32 nx, ny, nz
    voxel_mm  uint32               EDGE OF ONE VOXEL IN MILLIMETRES
    runs      uint32               number of run pairs
    body      runs x (uint8 material, uint32 count)

Iteration order is C order over [x][y][z], so z varies fastest.

WHY VERSION 2 EXISTS: A FILE HAS TO SAY WHAT SCALE IT IS
--------------------------------------------------------
Version 1 recorded a box of voxels and never said how big a voxel was. That was
survivable while every asset was on one lattice and it stopped being survivable
the moment they were not. The library now spans FOUR lattices -- 1 cm birds and
small fish, 2 cm tuna, 5 cm ground cover, 10 cm trees, rocks and large whales --
and a v1 file cannot tell them apart. Worse, the two readers disagreed about
what to assume: `AssetGrid::at` in voxel-core takes plain integer voxel
coordinates with no scale at all, so it necessarily reads an asset as terrain
lattice, while `vxa.read` here built a `VoxelGrid` with the default voxel size
and quietly discarded whatever the asset was actually baked at.

Neither is a diagnosable failure. A 5 cm boulder read as 10 cm is not an error,
it is a boulder at twice its size, in a world full of boulders.

V1 IS REFUSED RATHER THAN ASSUMED. The tempting compromise -- read a v1 file as
10 cm, since that is the terrain lattice -- is wrong for exactly the files that
exist: `granite-boulder` and `tundra-pine` were both baked v1 at 5 cm. An
assumption that is wrong for the actual corpus is not a fallback, it is the bug
with a friendlier face. There are seven baked assets in the library and
re-baking them costs seconds.
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np

from .grid import VoxelGrid

MAGIC = b"VXA1"
VERSION = 2

# 4 magic + 4 version + 12 origin + 12 size + 4 voxel_mm + 4 runs.
HEADER_BYTES = 40


def encode(grid: VoxelGrid) -> bytes:
    flat = grid.data.ravel(order="C")
    if flat.size == 0:
        runs_mat = np.empty(0, dtype=np.uint8)
        runs_len = np.empty(0, dtype=np.uint32)
    else:
        change = np.flatnonzero(flat[1:] != flat[:-1]) + 1
        starts = np.concatenate(([0], change))
        ends = np.concatenate((change, [flat.size]))
        runs_mat = flat[starts]
        runs_len = (ends - starts).astype(np.uint32)

    # Millimetres, and an integer, because the whole point is that the two
    # sides agree exactly. Every lattice in the library is a whole number of
    # millimetres (10, 20, 50, 100) and a fractional one would mean an asset
    # that cannot be placed on any integer grid, so refusing is better than
    # rounding silently.
    voxel_mm = grid.voxel_m * 1000.0
    if abs(voxel_mm - round(voxel_mm)) > 1e-6 or round(voxel_mm) <= 0:
        raise ValueError(
            f"voxel size {grid.voxel_m} m is not a whole number of millimetres; "
            f"a VXA file has to state a scale the engine can place on an "
            f"integer lattice")

    header = (
        MAGIC
        + struct.pack("<I", VERSION)
        + struct.pack("<iii", *(int(v) for v in grid.origin))
        + struct.pack("<III", *(int(v) for v in grid.shape))
        + struct.pack("<I", int(round(voxel_mm)))
        + struct.pack("<I", runs_mat.size)
    )
    body = np.empty(runs_mat.size, dtype=[("m", np.uint8), ("n", np.uint32)])
    body["m"] = runs_mat
    body["n"] = runs_len
    return header + body.tobytes()


def decode(blob: bytes) -> VoxelGrid:
    if blob[:4] != MAGIC:
        raise ValueError("not a VXA file")
    version = struct.unpack("<I", blob[4:8])[0]
    if version == 1:
        # Named, not lumped in with "unsupported". A v1 file is not corrupt --
        # it is a file that never recorded its own scale, and the fix is to
        # re-bake it rather than to guess. See the module docstring.
        raise ValueError(
            "VXA version 1 does not record a voxel size, so it cannot be "
            "placed: re-bake the asset (forge.server.keep) to get version 2")
    if version != VERSION:
        raise ValueError(f"unsupported VXA version {version}")
    ox, oy, oz, nx, ny, nz, voxel_mm, nruns = struct.unpack(
        "<iiiIIIII", blob[8:HEADER_BYTES])
    body = np.frombuffer(
        blob[HEADER_BYTES : HEADER_BYTES + nruns * 5],
        dtype=[("m", np.uint8), ("n", np.uint32)],
    )
    flat = np.repeat(body["m"], body["n"])
    # The voxel size is CARRIED THROUGH, not defaulted. `read` used to build a
    # grid at the default lattice, so a round trip through this format silently
    # changed a 1 cm bird into a 5 cm one and nothing downstream could tell.
    grid = VoxelGrid((nx, ny, nz), (ox, oy, oz), voxel_mm / 1000.0)
    grid.data[:] = flat.reshape((nx, ny, nz))
    return grid


def write(grid: VoxelGrid, path: str | Path) -> int:
    blob = encode(grid)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)
    return len(blob)


def read(path: str | Path) -> VoxelGrid:
    return decode(Path(path).read_bytes())
