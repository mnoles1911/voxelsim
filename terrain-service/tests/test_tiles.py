"""Tile pipeline tests, including the golden-tile regression (plan §5 task 1):
synthetic provider output is pinned by sha256 — any drift means dev worlds
stop reproducing and the provider version must be bumped instead."""

import hashlib

import numpy as np
import pytest

from terrain_service import tile_codec
from terrain_service.app import create_app
from terrain_service.cache import TileCache
from terrain_service.providers.synthetic import SyntheticProvider

# Pinned goldens for synthetic-v1. Regenerate ONLY on a deliberate provider
# version bump: python -c "from tests.test_tiles import print_goldens; print_goldens()"
GOLDEN_SHA256 = {
    (1, 0, 0, 1): "1779c62545c50554212e21fd38e7dccff4010f602f1cc8237e498e7b410857c3",
    (1, -2, 3, 1): "84ccdaf3125458b5139c58cc03418c58e6230f7836d1a5f5ca44051c3b31ade2",
    (20260719, 5, -7, 8): "7c6c13b5a28c74eb7d13fce159411aa0d9072ea707974a10b5c2ddd734894e72",
}


def tile_sha(seed, x, y, scale):
    tile = SyntheticProvider().generate(seed, x, y, scale)
    return hashlib.sha256(tile_codec.encode(tile)).hexdigest()


def print_goldens():
    for key in GOLDEN_SHA256:
        print(f"    {key}: \"{tile_sha(*key)}\",")


def test_codec_roundtrip():
    tile = SyntheticProvider().generate(42, 1, -1, 1)
    decoded = tile_codec.decode(tile_codec.encode(tile))
    assert decoded.seed == 42 and decoded.x == 1 and decoded.y == -1
    assert decoded.scale == 1
    np.testing.assert_array_equal(decoded.elevation, tile.elevation)
    np.testing.assert_array_equal(decoded.climate, tile.climate)


def test_codec_rejects_garbage():
    with pytest.raises(ValueError):
        tile_codec.decode(b"NOPE" + b"\0" * 100)
    good = tile_codec.encode(SyntheticProvider().generate(1, 0, 0, 1))
    with pytest.raises(ValueError):
        tile_codec.decode(good + b"\0")  # trailing bytes


def test_synthetic_deterministic_and_seamless():
    p = SyntheticProvider()
    a = p.generate(7, 2, 2, 1)
    b = p.generate(7, 2, 2, 1)
    np.testing.assert_array_equal(a.elevation, b.elevation)
    # Neighbor tiles must agree at the shared border (no tile-local seams):
    # right edge column of (2,2) continues into left edge of (3,2) smoothly —
    # verified by comparing against a straddling evaluation. Cheap proxy:
    # max step across the border stays within the max lattice slope.
    right = p.generate(7, 3, 2, 1)
    step = np.abs(
        a.elevation[:, -1].astype(np.int32) - right.elevation[:, 0].astype(np.int32)
    )
    assert step.max() <= 2 * 1400 * 2 // 32 + 5  # finest octave slope bound (m/px)


def test_golden_tiles():
    for (seed, x, y, scale), want in GOLDEN_SHA256.items():
        assert tile_sha(seed, x, y, scale) == want, (
            f"synthetic tile ({seed},{x},{y},s{scale}) drifted — if intentional, "
            "bump SyntheticProvider.provider_id and regenerate goldens"
        )


def test_cache_roundtrip(tmp_path):
    cache = TileCache(tmp_path)
    assert cache.get("p-v1", 1, 0, 0, 1) is None
    cache.put("p-v1", 1, 0, 0, 1, b"hello")
    assert cache.get("p-v1", 1, 0, 0, 1) == b"hello"
    # Different provider version never collides.
    assert cache.get("p-v2", 1, 0, 0, 1) is None


def test_tile_api(tmp_path):
    app = create_app(cache=TileCache(tmp_path))
    client = app.test_client()

    r = client.get("/tile?seed=1&x=0&y=0&scale=1")
    assert r.status_code == 200
    assert r.mimetype == "application/octet-stream"
    tile = tile_codec.decode(r.data)
    assert tile.elevation.shape == (512, 512)

    # Second request must be served byte-identical (from cache).
    r2 = client.get("/tile?seed=1&x=0&y=0&scale=1")
    assert r2.data == r.data

    assert client.get("/tile?seed=1&x=0").status_code == 400
    assert client.get("/tile?seed=1&x=0&y=0&scale=3").status_code == 400
    assert client.get("/healthz").status_code == 200
