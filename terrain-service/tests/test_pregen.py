"""Tests for the pre-generation CLI."""

import subprocess
from pathlib import Path

import numpy as np
import pytest

from terrain_service import tile_codec
from terrain_service.cache import TileCache
from terrain_service.providers.synthetic import SyntheticProvider

#: See test_diffusion.py's DRYRUN.
DRYRUN = "-dryrun-" + SyntheticProvider.provider_id


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


# ---------------------------------------------------------------------------
# --checkpoint-id / --checkpoint-sha256: the pinned-config wiring
# (docs/pod-bringup-commands.md Block 5's documented gap -- the CLI must not
# silently fall back to DiffusionConfig()'s UNPINNED default when a pinned
# checkpoint is given).
# ---------------------------------------------------------------------------


def test_pregen_diffusion_dry_run_uses_pinned_checkpoint_in_cache_key(tmp_path):
    from terrain_service.providers.diffusion import DiffusionConfig

    cache_dir = tmp_path / "cache"
    result = subprocess.run(
        [
            "python",
            "-m",
            "terrain_service.pregen",
            "--seed", "5",
            "--radius", "0",
            "--cache-dir", str(cache_dir),
            "--provider", "diffusion",
            "--dry-run",
            "--checkpoint-id", "ckpt-cli",
            "--checkpoint-sha256", "c" * 64,
        ],
        cwd=Path(__file__).parent.parent,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, f"pregen failed: {result.stderr}"
    assert "generated=1" in result.stderr

    # The tile must be cached under the PINNED config's provider_id -- proof
    # --checkpoint-id/--checkpoint-sha256 actually reached DiffusionProvider
    # via _make_provider(config=...), not just parsed and discarded.
    pinned_id = (
        DiffusionConfig(checkpoint_id="ckpt-cli", checkpoint_sha256="c" * 64, scale=1)
        .provider_id()
        + DRYRUN
    )
    cache = TileCache(cache_dir)
    assert cache.get(pinned_id, 5, 0, 0, 1) is not None


def test_pregen_diffusion_dry_run_without_pin_uses_unpinned_default(tmp_path):
    """Companion to the test above: omitting --checkpoint-id/--checkpoint-
    sha256 still works (dry-run only needs a config object to exist, not a
    pinned one) and caches under the plain UNPINNED-default provider_id --
    proving the two cases are genuinely distinguishable, not coincidentally
    identical."""
    from terrain_service.providers.diffusion import DiffusionConfig

    cache_dir = tmp_path / "cache"
    result = subprocess.run(
        [
            "python",
            "-m",
            "terrain_service.pregen",
            "--seed", "5",
            "--radius", "0",
            "--cache-dir", str(cache_dir),
            "--provider", "diffusion",
            "--dry-run",
        ],
        cwd=Path(__file__).parent.parent,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, f"pregen failed: {result.stderr}"

    default_id = DiffusionConfig(scale=1).provider_id() + DRYRUN
    cache = TileCache(cache_dir)
    assert cache.get(default_id, 5, 0, 0, 1) is not None


def test_pregen_diffusion_pins_conditioning_and_label_in_cache_key(tmp_path):
    """The new identity fields must reach DiffusionProvider, and the run must
    announce the namespace it writes into.

    checkpoint_label and conditioning_digest are what make the identity
    content-addressed (see providers/diffusion.py): the label replaces the
    load path that used to be embedded in provider_id, and the conditioning
    digest covers the WorldClim/ETOPO rasters that condition generation.
    """
    from terrain_service.providers.diffusion import DiffusionConfig

    cache_dir = tmp_path / "cache"
    result = subprocess.run(
        [
            "python",
            "-m",
            "terrain_service.pregen",
            "--seed", "5",
            "--radius", "0",
            "--cache-dir", str(cache_dir),
            "--provider", "diffusion",
            "--dry-run",
            "--checkpoint-id", "/some/mount/point",
            "--checkpoint-label", "terrain-diffusion-30m",
            "--checkpoint-sha256", "c" * 64,
            "--conditioning-digest", "d" * 64,
            "--terrain-diffusion-version", "abc1234",
        ],
        cwd=Path(__file__).parent.parent,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, f"pregen failed: {result.stderr}"
    assert "generated=1" in result.stderr

    pinned_id = (
        DiffusionConfig(
            checkpoint_id="/some/mount/point",
            checkpoint_label="terrain-diffusion-30m",
            checkpoint_sha256="c" * 64,
            conditioning_digest="d" * 64,
            terrain_diffusion_version="abc1234",
            scale=1,
        ).provider_id()
        + DRYRUN
    )
    cache = TileCache(cache_dir)
    assert cache.get(pinned_id, 5, 0, 0, 1) is not None
    # The run announces its namespace, and the mount point never appears in it.
    assert f"provider_id: {pinned_id}" in result.stderr
    assert "/some/mount/point" not in pinned_id


def test_pregen_rejects_a_path_as_checkpoint_label(tmp_path):
    """Putting the mount point in the identity field (the old habit that
    caused bug 1) must fail with a clear message, not silently work."""
    result = subprocess.run(
        [
            "python",
            "-m",
            "terrain_service.pregen",
            "--seed", "5",
            "--radius", "0",
            "--cache-dir", str(tmp_path / "cache"),
            "--provider", "diffusion",
            "--dry-run",
            "--checkpoint-label", "/workspace/ckpt/terrain-diffusion-30m",
        ],
        cwd=Path(__file__).parent.parent,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 1
    assert "checkpoint_label" in result.stderr


def test_pregen_print_conditioning_digest(tmp_path, monkeypatch):
    """The bring-up helper that produces the value for --conditioning-digest.
    With no conditioning data present it must refuse (exit 1) rather than
    print an invented digest."""
    import os

    from terrain_service.providers.diffusion import (
        DEFAULT_CONDITIONING_FILES,
        compute_conditioning_digest,
    )

    root = tmp_path / "global"
    root.mkdir()
    env = dict(os.environ, TERRAIN_CONDITIONING_ROOT=str(root))
    cmd = [
        "python", "-m", "terrain_service.pregen",
        "--seed", "5", "--radius", "0", "--print-conditioning-digest",
    ]

    missing = subprocess.run(
        cmd, cwd=Path(__file__).parent.parent, capture_output=True, text=True, env=env
    )
    assert missing.returncode == 1
    assert "etopo_10m.tif" in missing.stderr

    for name in DEFAULT_CONDITIONING_FILES:
        (root / name).write_bytes(name.encode())
    ok = subprocess.run(
        cmd, cwd=Path(__file__).parent.parent, capture_output=True, text=True, env=env
    )
    assert ok.returncode == 0, ok.stderr
    assert compute_conditioning_digest(root=root) in ok.stdout


# ---------------------------------------------------------------------------
# The superblock completeness PUBLISH gate.
#
# Before this existed, "INCOMPLETE (102 of 256 coarse tiles absent)" was printed
# and the tile was published anyway -- which is how the 2026-08-02 world got
# baked. The defect is permanent (a shipped tile is never regenerated) and in
# multiplayer it desyncs terrain between players who bake the same ground at
# different frontier sizes. See pregen.superblock_gate_verdict.
# ---------------------------------------------------------------------------


def test_superblock_gate_publishes_only_complete():
    from terrain_service.pregen import superblock_gate_verdict

    ok, msg = superblock_gate_verdict(0.0, allow_incomplete=False)
    assert ok is True
    assert msg == ""


@pytest.mark.parametrize("missing", [1.0, 102.0, 255.0])
def test_superblock_gate_refuses_incomplete(missing):
    from terrain_service.pregen import superblock_gate_verdict

    ok, msg = superblock_gate_verdict(missing, allow_incomplete=False)
    assert ok is False
    assert "refusing to publish" in msg
    assert f"{int(missing)} coarse tiles absent" in msg
    # The operator must be told how to proceed, not just refused.
    assert "--allow-incomplete-superblock" in msg


def test_superblock_gate_refuses_when_there_is_no_superblock_at_all():
    """A negative count means NO superblock, which must not read as 0 missing."""
    from terrain_service.pregen import superblock_gate_verdict

    ok, msg = superblock_gate_verdict(-1.0, allow_incomplete=False)
    assert ok is False
    assert "NO flow superblock at all" in msg


@pytest.mark.parametrize("missing", [-1.0, 1.0, 102.0])
def test_superblock_gate_override_publishes_but_says_do_not_ship(missing):
    from terrain_service.pregen import superblock_gate_verdict

    ok, msg = superblock_gate_verdict(missing, allow_incomplete=True)
    assert ok is True
    assert "do NOT ship this tile" in msg


def test_superblock_gate_message_carries_tile_coordinates():
    """The caller formats {x}/{y} in; a gate that cannot name the tile is noise."""
    from terrain_service.pregen import superblock_gate_verdict

    _, msg = superblock_gate_verdict(7.0, allow_incomplete=False)
    assert msg.format(x=-3, y=-11).startswith("error: tile (-3,-11)")


def test_allow_incomplete_superblock_is_advertised_in_help():
    out = subprocess.run(
        ["python", "-m", "terrain_service.pregen", "--help"],
        cwd=Path(__file__).parent.parent, capture_output=True, text=True,
    )
    assert out.returncode == 0
    assert "--allow-incomplete-superblock" in out.stdout
    assert "DEVELOPMENT ONLY" in out.stdout


# ---------------------------------------------------------------------------
# --codec. _encode_fine has accepted a codec since 2026-08-01 but no caller
# passed one, so every fine tile ever written by pregen is uncompressed.
# ---------------------------------------------------------------------------


def test_resolve_codec_raw_is_the_default():
    from terrain_service import tile_codec as tc
    from terrain_service.pregen import _resolve_codec

    assert _resolve_codec("raw") == tc.CODEC_RAW
    assert _resolve_codec("") == tc.CODEC_RAW
    assert _resolve_codec(None) == tc.CODEC_RAW


def test_resolve_codec_zstd_or_a_clear_refusal():
    """Never silently degrade: a cache full of RAW under a zstd flag is worse
    than a failed run, because the size only shows up as a bandwidth bill."""
    from terrain_service import tile_codec as tc
    from terrain_service.pregen import _resolve_codec

    if tc.HAVE_ZSTD:
        assert _resolve_codec("zstd") == tc.CODEC_ZSTD
    else:
        with pytest.raises(SystemExit) as e:
            _resolve_codec("zstd")
        assert "zstandard" in str(e.value)


def test_resolve_codec_rejects_nonsense():
    from terrain_service.pregen import _resolve_codec

    with pytest.raises(SystemExit):
        _resolve_codec("lz4")


def test_codec_flag_is_advertised_and_has_no_auto():
    out = subprocess.run(
        ["python", "-m", "terrain_service.pregen", "--help"],
        cwd=Path(__file__).parent.parent, capture_output=True, text=True,
    )
    assert out.returncode == 0
    assert "--codec" in out.stdout
    # 'auto' would defeat the whole point; assert it is not offered.
    assert "auto" not in out.stdout.split("--codec")[1].split("--")[0]
