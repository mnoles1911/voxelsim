"""Synthetic tile provider — DEV ONLY, NOT CANONICAL WORLD DATA.

Deterministic integer value-noise terrain for developing/testing the tile
pipeline without a GPU. The real provider is terrain-diffusion
(providers/diffusion.py). Because tiles are data (doctrine §2.3), this
provider does NOT need to match voxel-core's C++ synthetic sampler — but its
own output must be reproducible forever, hence pure integer math and a
versioned provider_id in the cache key.
"""

from __future__ import annotations

import numpy as np

from ..tile_codec import CLIMATE_CHANNELS, TILE_SIZE, Tile

U64 = np.uint64
_GOLDEN = U64(0x9E3779B97F4A7C15)
_MIX1 = U64(0xBF58476D1CE4E5B9)
_MIX2 = U64(0x94D049BB133111EB)


def _splitmix64(z: np.ndarray) -> np.ndarray:
    z = z + _GOLDEN
    z = (z ^ (z >> U64(30))) * _MIX1
    z = (z ^ (z >> U64(27))) * _MIX2
    return z ^ (z >> U64(31))


def _hash2(seed: int, x: np.ndarray, y: np.ndarray, channel: int) -> np.ndarray:
    h = _splitmix64(np.full_like(x, channel).astype(U64))
    h = _splitmix64(y.astype(np.int64).astype(U64) ^ h)
    h = _splitmix64(x.astype(np.int64).astype(U64) ^ h)
    return _splitmix64(U64(seed) ^ h)


def _corner(seed: int, x0: np.ndarray, y0: np.ndarray, channel: int) -> np.ndarray:
    """Lattice corner value in [-32768, 32767]."""
    return (_hash2(seed, x0, y0, channel) >> U64(48)).astype(np.int64) - 32768


def _value_noise(
    seed: int, gx: np.ndarray, gy: np.ndarray, lattice: int, channel: int
) -> np.ndarray:
    """Exact integer bilinear value noise over global pixel coords (int64)."""
    x0, y0 = gx // lattice, gy // lattice  # numpy // floors, incl. negatives
    fx, fy = gx - x0 * lattice, gy - y0 * lattice
    v00 = _corner(seed, x0, y0, channel)
    v10 = _corner(seed, x0 + 1, y0, channel)
    v01 = _corner(seed, x0, y0 + 1, channel)
    v11 = _corner(seed, x0 + 1, y0 + 1, channel)
    gxw, gyw = lattice - fx, lattice - fy
    return ((v00 * gxw + v10 * fx) * gyw + (v01 * gxw + v11 * fx) * fy) // (
        lattice * lattice
    )


class SyntheticProvider:
    provider_id = "synthetic-v1"

    def generate(self, seed: int, x: int, y: int, scale: int) -> Tile:
        px = np.arange(TILE_SIZE, dtype=np.int64) + x * TILE_SIZE
        py = np.arange(TILE_SIZE, dtype=np.int64) + y * TILE_SIZE
        gx, gy = np.meshgrid(px, py)  # row-major: gy varies along axis 0

        # Continental-scale octaves (amplitudes in metres), slight negative
        # bias for oceans/coastlines — same character as the C++ dev sampler.
        n0 = _value_noise(seed, gx, gy, 512, 100)
        n1 = _value_noise(seed, gx, gy, 128, 101)
        n2 = _value_noise(seed, gx, gy, 32, 102)
        elev_m = (n0 * 1400 + n1 * 450 + n2 * 130) // 32768 - 120
        elevation = np.clip(elev_m, -32768, 32767).astype(np.int16)

        climate = np.zeros((CLIMATE_CHANNELS, TILE_SIZE, TILE_SIZE), dtype=np.uint8)
        t = _value_noise(seed, gx, gy, 1024, 103)
        s = _value_noise(seed, gx, gy, 512, 104)
        p = _value_noise(seed, gx, gy, 768, 105)
        v = _value_noise(seed, gx, gy, 384, 106)
        climate[0] = np.clip(128 + t // 256, 0, 255).astype(np.uint8)  # temperature
        climate[1] = np.clip(128 + s // 512, 0, 255).astype(np.uint8)  # seasonality
        climate[2] = np.clip(128 + p // 256, 0, 255).astype(np.uint8)  # precipitation
        climate[3] = np.clip(128 + v // 512, 0, 255).astype(np.uint8)  # precip var.
        return Tile(seed=seed, x=x, y=y, scale=scale, elevation=elevation, climate=climate)
