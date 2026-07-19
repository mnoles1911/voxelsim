"""Binary tile format (plan §3.1 step 2, §5 task 1).

Tiles are canonical DATA (doctrine §2.3): generated once server-side,
cached forever, distributed to clients. 512x512 pixels per tile:

    elevation: int16 metres, signed, z=0 is sea level
    climate:   uint8 x 4 planes (temperature, seasonality,
               precipitation, precip variability)

Wire format (little-endian throughout):

    magic    4B  b"VXTL"
    version  u16 (1)
    seed     u64
    x, y     i32 tile coords (tile (0,0) covers pixels [0,512) on each axis)
    scale    u8  (1 => 30m/px, 8 => 11.25m/px, terrain-diffusion convention)
    size     u16 (512)
    elevation int16[size*size], row-major (y outer)
    climate   uint8[4][size*size]
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np

MAGIC = b"VXTL"
VERSION = 1
TILE_SIZE = 512
CLIMATE_CHANNELS = 4
_HEADER = struct.Struct("<4sHQiiBH")

#: mm per pixel by scale — must match voxel-core's ITileSampler::pixelSizeMm.
PIXEL_SIZE_MM = {1: 30000, 8: 11250}


@dataclass
class Tile:
    seed: int
    x: int
    y: int
    scale: int
    elevation: np.ndarray  # (TILE_SIZE, TILE_SIZE) int16
    climate: np.ndarray  # (CLIMATE_CHANNELS, TILE_SIZE, TILE_SIZE) uint8

    def __post_init__(self) -> None:
        assert self.scale in PIXEL_SIZE_MM, f"unsupported scale {self.scale}"
        assert self.elevation.shape == (TILE_SIZE, TILE_SIZE)
        assert self.elevation.dtype == np.int16
        assert self.climate.shape == (CLIMATE_CHANNELS, TILE_SIZE, TILE_SIZE)
        assert self.climate.dtype == np.uint8


def encode(tile: Tile) -> bytes:
    header = _HEADER.pack(
        MAGIC, VERSION, tile.seed, tile.x, tile.y, tile.scale, TILE_SIZE
    )
    return (
        header
        + tile.elevation.astype("<i2").tobytes()
        + tile.climate.tobytes()
    )


def decode(data: bytes) -> Tile:
    magic, version, seed, x, y, scale, size = _HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ValueError("bad magic")
    if version != VERSION:
        raise ValueError(f"unsupported tile version {version}")
    if size != TILE_SIZE:
        raise ValueError(f"unsupported tile size {size}")
    offset = _HEADER.size
    n = size * size
    elevation = np.frombuffer(data, dtype="<i2", count=n, offset=offset)
    offset += 2 * n
    climate = np.frombuffer(
        data, dtype=np.uint8, count=CLIMATE_CHANNELS * n, offset=offset
    )
    if offset + CLIMATE_CHANNELS * n != len(data):
        raise ValueError("trailing bytes")
    return Tile(
        seed=seed,
        x=x,
        y=y,
        scale=scale,
        elevation=elevation.reshape(size, size).astype(np.int16),
        climate=climate.reshape(CLIMATE_CHANNELS, size, size).copy(),
    )
