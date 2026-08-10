"""MagicaVoxel .vox export, for the Blender and MagicaVoxel round trip.

Two things about the format matter here. Voxel coordinates are single bytes, so
no model can exceed 256 on an axis -- and a 30 m jungle emergent is 300 voxels
tall. Large trees are therefore split into a grid of models placed by a scene
graph, which is how MagicaVoxel handles large scenes itself.

Palette indices are the material IDs, so index 16 in an exported file is bark
and index 19 is broadleaf. That makes a .vox file readable without this tool,
and it means a designer's edit in MagicaVoxel comes back with its materials
intact as long as they stay on their palette slots.

Format reference: https://github.com/ephtracy/voxel-model/blob/master/MagicaVoxel-file-format-vox.txt
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np

from . import materials
from .grid import VoxelGrid

MAX_MODEL = 256


def _chunk(cid: bytes, content: bytes = b"", children: bytes = b"") -> bytes:
    return cid + struct.pack("<ii", len(content), len(children)) + content + children


def _string(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<i", len(b)) + b


def _dict(d: dict[str, str]) -> bytes:
    out = struct.pack("<i", len(d))
    for k, v in d.items():
        out += _string(k) + _string(v)
    return out


def _palette() -> bytes:
    """256 RGBA entries. File entry i holds palette index i+1."""
    table = bytearray(256 * 4)
    for mat, rgb in materials.COLORS.items():
        if mat == 0 or mat > 255:
            continue
        off = (mat - 1) * 4
        table[off : off + 4] = bytes((*rgb, 255))
    return bytes(table)


def _blocks(grid: VoxelGrid):
    """Split into models no larger than 256 on any axis."""
    nx, ny, nz = grid.shape
    for x0 in range(0, nx, MAX_MODEL):
        for y0 in range(0, ny, MAX_MODEL):
            for z0 in range(0, nz, MAX_MODEL):
                sub = grid.data[
                    x0 : min(x0 + MAX_MODEL, nx),
                    y0 : min(y0 + MAX_MODEL, ny),
                    z0 : min(z0 + MAX_MODEL, nz),
                ]
                if np.any(sub):
                    yield (x0, y0, z0), sub


def write(grid: VoxelGrid, path: str | Path, name: str = "tree") -> int:
    """Write the grid as .vox. Returns the number of models emitted."""
    blocks = list(_blocks(grid))
    if not blocks:
        blocks = [((0, 0, 0), np.zeros((1, 1, 1), dtype=np.uint8))]

    body = b""
    for _, sub in blocks:
        sx, sy, sz = sub.shape
        body += _chunk(b"SIZE", struct.pack("<iii", sx, sy, sz))
        xs, ys, zs = np.nonzero(sub)
        cols = sub[xs, ys, zs]
        voxels = np.empty((xs.size, 4), dtype=np.uint8)
        voxels[:, 0] = xs
        voxels[:, 1] = ys
        voxels[:, 2] = zs
        voxels[:, 3] = cols
        body += _chunk(
            b"XYZI", struct.pack("<i", int(xs.size)) + voxels.tobytes(order="C")
        )

    # Scene graph: root transform -> group -> one transform+shape per model.
    # MagicaVoxel places a model centred on its transform, so the offset is the
    # block's origin plus half its size.
    n = len(blocks)
    child_ids = [2 + 2 * i for i in range(n)]
    graph = _chunk(
        b"nTRN",
        struct.pack("<i", 0)
        + _dict({"_name": name})
        + struct.pack("<iiii", 1, -1, -1, 1)
        + _dict({}),
    )
    graph += _chunk(
        b"nGRP",
        struct.pack("<i", 1)
        + _dict({})
        + struct.pack("<i", n)
        + b"".join(struct.pack("<i", c) for c in child_ids),
    )
    for i, ((x0, y0, z0), sub) in enumerate(blocks):
        sx, sy, sz = sub.shape
        tx, ty, tz = x0 + sx // 2, y0 + sy // 2, z0 + sz // 2
        graph += _chunk(
            b"nTRN",
            struct.pack("<i", child_ids[i])
            + _dict({})
            + struct.pack("<iiii", child_ids[i] + 1, -1, -1, 1)
            + _dict({"_t": f"{tx} {ty} {tz}"}),
        )
        graph += _chunk(
            b"nSHP",
            struct.pack("<i", child_ids[i] + 1)
            + _dict({})
            + struct.pack("<i", 1)
            + struct.pack("<i", i)
            + _dict({}),
        )

    children = body + graph + _chunk(b"RGBA", _palette())
    data = b"VOX " + struct.pack("<i", 150) + _chunk(b"MAIN", b"", children)

    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return n


def inspect(path: str | Path) -> dict:
    """Walk a .vox file back and report what is actually in it.

    A minimal reader, here so that "the export works" can be checked rather
    than assumed -- a malformed chunk length writes happily and only fails when
    a designer opens the file.
    """
    blob = Path(path).read_bytes()
    if blob[:4] != b"VOX ":
        raise ValueError("not a .vox file")
    version = struct.unpack("<i", blob[4:8])[0]

    models: list[tuple[int, int, int]] = []
    voxels = 0
    chunks: dict[str, int] = {}
    palette_entries = 0

    def walk(buf: bytes, start: int, end: int) -> None:
        nonlocal voxels, palette_entries
        pos = start
        while pos < end:
            cid = buf[pos : pos + 4].decode("ascii", "replace")
            content_n, children_n = struct.unpack("<ii", buf[pos + 4 : pos + 12])
            body = pos + 12
            chunks[cid] = chunks.get(cid, 0) + 1
            if cid == "SIZE":
                models.append(struct.unpack("<iii", buf[body : body + 12]))
            elif cid == "XYZI":
                voxels += struct.unpack("<i", buf[body : body + 4])[0]
            elif cid == "RGBA":
                palette_entries = content_n // 4
            if children_n:
                walk(buf, body + content_n, body + content_n + children_n)
            pos = body + content_n + children_n

    walk(blob, 8, len(blob))
    return {
        "version": version,
        "models": models,
        "voxels": voxels,
        "chunks": chunks,
        "palette_entries": palette_entries,
        "bytes": len(blob),
        "oversized": [m for m in models if max(m) > MAX_MODEL],
    }
