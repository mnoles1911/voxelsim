"""Tests for the pre-generation CLI."""

import subprocess
from pathlib import Path

import numpy as np
import pytest

from terrain_service import tile_codec
from terrain_service.cache import TileCache
from terrain_service.providers.synthetic import SyntheticProvider


def test_pregen_radius1_generates_9_tiles(tmp_path):
    """Radius 1 should generate 9 tiles in a 3x3 square."""
    cache_dir = tmp_path / "cache"

    result = subprocess.run(
        [
            "python",
            "-m",
            "terrain_service.pregen",
            "--seed", "42",
            "--radius", "1",
            "--cache-dir", str(cache_dir),
            "--provider", "synthetic",
        ],
        cwd=Path(__file__).parent.parent,  # run from terrain-service dir
        capture_output=True,
        text=True,
    )

    assert result.returncode == 0, f"pregen failed: {result.stderr}"
    assert "generated=9" in result.stderr, f"unexpected output: {result.stderr}"

    # Verify all 9 tiles are cached
    cache = TileCache(cache_dir)
    provider = SyntheticProvider()
    count = 0
    for dx in range(-1, 2):
        for dy in range(-1, 2):
            x, y = 0 + dx, 0 + dy
            data = cache.get(provider.provider_id, 42, x, y, 1)
            assert data is not None, f"tile ({x},{y}) not cached"
            count += 1

    assert count == 9


def test_pregen_second_run_skips_all(tmp_path):
    """Running pregen twice should skip all cached tiles on the second run."""
    cache_dir = tmp_path / "cache"

    # First run: generate
    result1 = subprocess.run(
        [
            "python",
            "-m",
            "terrain_service.pregen",
            "--seed", "99",
            "--radius", "1",
            "--cache-dir", str(cache_dir),
            "--provider", "synthetic",
        ],
        cwd=Path(__file__).parent.parent,
        capture_output=True,
        text=True,
    )
    assert result1.returncode == 0
    assert "generated=9" in result1.stderr

    # Second run: should skip all
    result2 = subprocess.run(
        [
            "python",
            "-m",
            "terrain_service.pregen",
            "--seed", "99",
            "--radius", "1",
            "--cache-dir", str(cache_dir),
            "--provider", "synthetic",
        ],
        cwd=Path(__file__).parent.parent,
        capture_output=True,
        text=True,
    )
    assert result2.returncode == 0
    assert "generated=0" in result2.stderr, f"expected all tiles skipped: {result2.stderr}"
    assert "skipped=9" in result2.stderr


def test_pregen_cached_tile_roundtrips(tmp_path):
    """Cached tiles should decode correctly."""
    cache_dir = tmp_path / "cache"

    result = subprocess.run(
        [
            "python",
            "-m",
            "terrain_service.pregen",
            "--seed", "7",
            "--radius", "1",
            "--center-x", "5",
            "--center-y", "-3",
            "--cache-dir", str(cache_dir),
            "--provider", "synthetic",
        ],
        cwd=Path(__file__).parent.parent,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0

    # Decode one cached tile and verify it matches the generated tile
    cache = TileCache(cache_dir)
    provider = SyntheticProvider()

    x, y, scale = 5, -3, 1
    cached_data = cache.get(provider.provider_id, 7, x, y, scale)
    assert cached_data is not None

    # Decode from cache
    decoded = tile_codec.decode(cached_data)

    # Compare with freshly generated
    fresh = provider.generate(7, x, y, scale)

    assert decoded.seed == fresh.seed
    assert decoded.x == fresh.x
    assert decoded.y == fresh.y
    assert decoded.scale == fresh.scale
    np.testing.assert_array_equal(decoded.elevation, fresh.elevation)
    np.testing.assert_array_equal(decoded.climate, fresh.climate)
