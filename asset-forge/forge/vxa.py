"""VXA1 -- the compact native asset format.

What actually ships to the engine. Deliberately trivial to read from C++: a
fixed header, then run-length pairs of (material, count) over the grid in
x-major order. No palette (material IDs are the engine's own), no compression
beyond the runs, no scene graph.

A tree is mostly air inside its bounding box, so runs do the heavy lifting: a
typical broadleaf lands around 3-5% of its dense size.

    magic    "VXA1"                4 bytes
    version  uint32                = 1
    origin   int32 x, y, z         voxel offset from the tree's base, which is
                                   at (0, 0, 0) with z up
    size     uint32 nx, ny, nz
    runs     uint32                number of run pairs
    body     runs x (uint8 material, uint32 count)

Iteration order is C order over [x][y][z], so z varies fastest.
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np

from .grid import VoxelGrid

MAGIC = b"VXA1"
VERSION = 1


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

    header = (
        MAGIC
        + struct.pack("<I", VERSION)
        + struct.pack("<iii", *(int(v) for v in grid.origin))
        + struct.pack("<III", *(int(v) for v in grid.shape))
        + struct.pack("<I", runs_mat.size)
    )
    body = np.empty(runs_mat.size, dtype=[("m", np.uint8), ("n", np.uint32)])
    body["m"] = runs_mat
    body["n"] = runs_len
    return header + body.tobytes()


def decode(blob: bytes) -> VoxelGrid:
    if blob[:4] != MAGIC:
        raise ValueError("not a VXA1 file")
    version, ox, oy, oz, nx, ny, nz, nruns = struct.unpack("<IiiiIIII", blob[4:36])
    if version != VERSION:
        raise ValueError(f"unsupported VXA version {version}")
    body = np.frombuffer(
        blob[36 : 36 + nruns * 5], dtype=[("m", np.uint8), ("n", np.uint32)]
    )
    flat = np.repeat(body["m"], body["n"])
    grid = VoxelGrid((nx, ny, nz), (ox, oy, oz))
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
