"""VXA -- the compact native asset format.

What actually ships to the engine. Deliberately trivial to read from C++: a
fixed header, then run-length pairs of (material, count) over the grid in
x-major order. No palette (material IDs are the engine's own), no compression
beyond the runs, no scene graph.

A tree is mostly air inside its bounding box, so runs do the heavy lifting: a
typical broadleaf lands around 3-5% of its dense size.

    magic      "VXA1"              4 bytes  (unchanged across versions)
    version    uint32              = 3
    origin     int32 x, y, z       voxel offset from the asset's base, which is
                                   at (0, 0, 0) with z up
    size       uint32 nx, ny, nz
    voxel_mm   uint32              EDGE OF ONE VOXEL IN MILLIMETRES
    runs       uint32              number of material run pairs
    part_runs  uint32              number of part run pairs; 0 when the asset
                                   has no moving parts (a rock, a tree)
    joints     uint32              number of joint records; 0 likewise
    body       runs      x (uint8 material, uint32 count)
               part_runs x (uint8 part,     uint32 count)
               joints    x (uint8 part, uint8 parent, int32 x, y, z in mm)

Iteration order is C order over [x][y][z], so z varies fastest. The part runs
tile the same box as the material runs, in the same order.

WHY VERSION 3 EXISTS: RIGGING
-----------------------------
Animals are rigid-part animated and ship in ONE pose (owner, 2026-08-14; see
`docs/animal-rigging-decision.md`). The runtime rotates a wing about a shoulder
rather than swapping to a differently-baked bird, and it can do neither without
two facts a v2 file cannot state: which voxels are the wing, and where the
shoulder is.

Both are already known at bake time and were being discarded. The generators
tag every voxel by part so the paint pass can tell a wing from a body
(`forge/parts.py`), and a joint is the centroid of the voxels where a part
touches its parent face to face -- a shoulder IS where the wing meets the body,
so it is measured off the shipped voxels rather than reported separately and
free to drift.

Joint positions are in MILLIMETRES from the asset's local origin, not voxels.
A joint is generally not on a voxel centre -- it is the middle of a contact
patch -- and rounding it to the lattice would move a shoulder by up to half a
voxel, which on a 22 cm squirrel is most of a leg.

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
VERSION = 3

# 4 magic + 4 version + 12 origin + 12 size + 4 voxel_mm + 4 runs
# + 4 part_runs + 4 joints.
HEADER_BYTES = 48
JOINT_BYTES = 14   # uint8 part + uint8 parent + 3 x int32 mm, packed


def _runs(flat: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    if flat.size == 0:
        return np.empty(0, np.uint8), np.empty(0, np.uint32)
    change = np.flatnonzero(flat[1:] != flat[:-1]) + 1
    starts = np.concatenate(([0], change))
    ends = np.concatenate((change, [flat.size]))
    return flat[starts], (ends - starts).astype(np.uint32)


def encode(grid: VoxelGrid, parts=None, joints=()) -> bytes:
    """Bytes for one baked asset.

    `parts` is a uint8 array parallel to `grid.data` naming which part owns each
    voxel, and `joints` the records `forge.parts.joints` produced. Both optional:
    a rock has neither, and passing neither writes a file whose part_runs and
    joint counts are zero rather than a different format.
    """
    flat = grid.data.ravel(order="C")
    runs_mat, runs_len = _runs(flat)

    # The part runs are their OWN run-length encoding over the same box, not a
    # re-use of the material boundaries. A wing and the body beside it are often
    # the same material, so a material run happily spans the join -- encoding
    # parts against those boundaries would put two parts in one run and there is
    # no way to say that.
    if parts is not None:
        if parts.shape != grid.data.shape:
            raise ValueError(
                f"part tags are {parts.shape}, voxels are {grid.data.shape}; "
                f"they have to be the same box or a part id means nothing")
        part_mat, part_len = _runs(parts.ravel(order="C"))
    else:
        part_mat, part_len = np.empty(0, np.uint8), np.empty(0, np.uint32)

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
        + struct.pack("<I", part_mat.size)
        + struct.pack("<I", len(joints))
    )

    def pack(mats, lens):
        rec = np.empty(mats.size, dtype=[("m", np.uint8), ("n", np.uint32)])
        rec["m"] = mats
        rec["n"] = lens
        return rec.tobytes()

    mm = float(round(voxel_mm))
    jb = b""
    for j in joints:
        o = j["origin"]
        if o is None:
            # A part that touches its parent only at a corner has no joint to
            # write. Skipped rather than written as (0,0,0), which would put a
            # shoulder at the asset's corner and look like a rig bug rather
            # than the geometry bug it is. `forge.parts.joints` reports it.
            continue
        jb += struct.pack("<BBiii", int(j["part"]), int(j["parent"]),
                          int(round(o[0] * mm)), int(round(o[1] * mm)),
                          int(round(o[2] * mm)))
    # The count in the header has to match what was actually written.
    header = header[:-4] + struct.pack("<I", len(jb) // JOINT_BYTES)
    return header + pack(runs_mat, runs_len) + pack(part_mat, part_len) + jb


def decode(blob: bytes) -> VoxelGrid:
    if blob[:4] != MAGIC:
        raise ValueError("not a VXA file")
    version = struct.unpack("<I", blob[4:8])[0]
    if version in (1, 2):
        # Named, not lumped in with "unsupported". Neither is corrupt: a v1 file
        # never recorded its own scale and a v2 file never recorded its parts.
        # The fix differs, so the message says which -- v1 can be converted by
        # tools/vxa_upgrade.py because the voxels are all there, but v2 cannot
        # gain parts without the generator that knew them, so it must be
        # re-baked.
        what = ("a voxel size" if version == 1
                else "part tags or joints, so it cannot be rigged")
        how = ("tools/vxa_upgrade.py converts it in place"
               if version == 1 else "re-bake the asset (forge.server.keep)")
        raise ValueError(f"VXA version {version} does not record {what}: {how}")
    if version != VERSION:
        raise ValueError(f"unsupported VXA version {version}")
    ox, oy, oz, nx, ny, nz, voxel_mm, nruns, npart, njoint = struct.unpack(
        "<iiiIIIIIII", blob[8:HEADER_BYTES])

    at = HEADER_BYTES
    body = np.frombuffer(blob[at : at + nruns * 5],
                         dtype=[("m", np.uint8), ("n", np.uint32)])
    at += nruns * 5
    flat = np.repeat(body["m"], body["n"])
    # The voxel size is CARRIED THROUGH, not defaulted. `read` used to build a
    # grid at the default lattice, so a round trip through this format silently
    # changed a 1 cm bird into a 5 cm one and nothing downstream could tell.
    grid = VoxelGrid((nx, ny, nz), (ox, oy, oz), voxel_mm / 1000.0)
    grid.data[:] = flat.reshape((nx, ny, nz))

    parts = None
    if npart:
        prec = np.frombuffer(blob[at : at + npart * 5],
                             dtype=[("m", np.uint8), ("n", np.uint32)])
        at += npart * 5
        parts = np.repeat(prec["m"], prec["n"]).reshape((nx, ny, nz))

    joints = []
    mm = float(voxel_mm)
    for i in range(njoint):
        pid, par, jx, jy, jz = struct.unpack(
            "<BBiii", blob[at : at + JOINT_BYTES])
        at += JOINT_BYTES
        joints.append({"part": pid, "parent": par,
                       "origin": (jx / mm, jy / mm, jz / mm)})
    return grid, parts, joints


def write(grid: VoxelGrid, path: str | Path, parts=None, joints=()) -> int:
    blob = encode(grid, parts, joints)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(blob)
    return len(blob)


def read(path: str | Path) -> VoxelGrid:
    """The voxels only. Callers that want the rig use `read_full`."""
    return decode(Path(path).read_bytes())[0]


def read_full(path: str | Path):
    """(grid, parts, joints). `parts` is None and `joints` empty for anything
    with nothing to move."""
    return decode(Path(path).read_bytes())
