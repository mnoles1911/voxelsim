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
# THE WORLD IDENTITY RECORD.
#
# 2026-08-03: the same seed, repo commit and checkpoint bundle id produced a
# different world on a fresh pod, and the 289-tile world carrying every
# owner-approved vista can never be extended -- because its identity triple was
# never written down. Two of the six conditioning files are BUILT by
# bootstrap_pod.sh rather than downloaded and came out different there. See
# world_manifest.py and
# docs/measurements/world-identity-not-reproducible-2026-08-03.txt.
#
# world_identity_verdict is pure for the same reason superblock_gate_verdict
# is: the policy must be testable without a GPU or 25 MB of rasters.
# ---------------------------------------------------------------------------

MANIFEST_NAME = "world-identity.json"


def _manifest(identity=None, **meta):
    """A manifest as build_manifest would produce one, for verdict tests."""
    from terrain_service.world_manifest import build_manifest

    base = {
        "namespace_id": "terrain-diffusion-30m-abc123",
        "provider_id": "terrain-diffusion-30m-abc123",
        "seed": 20260719,
        "seed_hex": "000000000135276f",
        "checkpoint_sha256": "bc5c" * 16,
        "conditioning_digest": "c49e" * 16,
        "terrain_diffusion_version": "82a0431+worldgen.c55a6382c524",
        "conditioning_file_sha256": {
            "etopo_10m.tif": "9a45dd6dc5b0959c",
            "synthetic_map_stats.json": "8fe9d083f10a0daf",
            "wc2.1_10m_bio_1.tif": "f61633ffac9d44ed",
        },
    }
    base.update(identity or {})
    m = build_manifest(base)
    m.update(meta)
    return m


def test_world_identity_verdict_agrees_with_itself():
    from terrain_service.world_manifest import world_identity_verdict

    ok, msg = world_identity_verdict(_manifest(), _manifest())
    assert ok is True
    assert msg == ""


def test_world_identity_verdict_ignores_the_timestamp():
    """Metadata is not identity: a world extended tomorrow is the same world."""
    from terrain_service.world_manifest import world_identity_verdict

    old = _manifest()
    old["created_utc"] = "2026-07-19T04:00:00Z"
    ok, msg = world_identity_verdict(old, _manifest())
    assert (ok, msg) == (True, "")


def test_world_identity_verdict_refuses_a_different_checkpoint():
    from terrain_service.world_manifest import world_identity_verdict

    ok, msg = world_identity_verdict(
        _manifest(), _manifest({"checkpoint_sha256": "dead" * 16})
    )
    assert ok is False
    assert "checkpoint_sha256" in msg
    assert "dead" * 16 in msg


def test_world_identity_verdict_names_the_conditioning_file_that_moved():
    """The 2026-08-03 finding in one assertion: that the digest moved is not
    news, WHICH of the six files moved it is. Four were byte-identical on both
    machines; the two that differed were the two the bootstrap builds."""
    from terrain_service.world_manifest import world_identity_verdict

    drifted = dict(_manifest()["identity"]["conditioning_file_sha256"])
    drifted["etopo_10m.tif"] = "3a0e8ebe4d54b15c"  # what the fresh pod built
    ok, msg = world_identity_verdict(
        _manifest(), _manifest({"conditioning_file_sha256": drifted})
    )
    assert ok is False
    assert "conditioning_file_sha256[etopo_10m.tif]" in msg
    assert "9a45dd6dc5b0959c" in msg and "3a0e8ebe4d54b15c" in msg
    # The other files agreed and must not be reported as if they had not.
    assert "wc2.1_10m_bio_1.tif" not in msg


def test_world_identity_verdict_never_suggests_forcing_the_namespace():
    """--provider-id-override would silence this by putting two planets in one
    namespace, with a seam in the middle and no error anywhere. The refusal
    must not read as an invitation to reach for it."""
    from terrain_service.world_manifest import world_identity_verdict

    _, msg = world_identity_verdict(
        _manifest(), _manifest({"checkpoint_sha256": "dead" * 16})
    )
    assert "--provider-id-override" not in msg
    assert "Do not force the two together" in msg
    # ...and it must say what to do instead.
    assert "NEW world" in msg


def test_world_identity_verdict_message_carries_the_world_directory():
    """The caller formats {world} in; a verdict that cannot name the world it
    refused is noise (the same contract as superblock_gate_verdict's {x}/{y})."""
    from terrain_service.world_manifest import world_identity_verdict

    _, msg = world_identity_verdict(
        _manifest(), _manifest({"seed": 1, "seed_hex": "0" * 16})
    )
    assert "tile-cache/w/0000" in msg.format(world="tile-cache/w/0000")


def test_world_identity_verdict_warns_but_allows_when_a_field_is_one_sided():
    """A bake extends a world from cached coarse tiles and never opens the
    conditioning rasters, so it may have none to hash. Nothing conflicts: say
    so loudly and continue."""
    from terrain_service.world_manifest import world_identity_verdict

    bake_side = _manifest()
    del bake_side["identity"]["conditioning_file_sha256"]
    ok, msg = world_identity_verdict(_manifest(), bake_side)
    assert ok is True
    assert "could not verify" in msg
    assert "conditioning_file_sha256[etopo_10m.tif]" in msg


def test_world_identity_verdict_is_silent_when_neither_side_records_a_field():
    """The synthetic provider's whole identity IS its provider_id. Absent on
    both sides is agreement, not a gap -- warning about it on every run would
    train operators to ignore the warning that matters."""
    from terrain_service.world_manifest import build_manifest, world_identity_verdict

    ident = {"namespace_id": "synthetic-v1", "provider_id": "synthetic-v1",
             "seed": 42, "seed_hex": f"{42:016x}"}
    ok, msg = world_identity_verdict(build_manifest(ident), build_manifest(ident))
    assert (ok, msg) == (True, "")


def test_world_identity_verdict_refuses_a_schema_it_cannot_read():
    from terrain_service.world_manifest import world_identity_verdict

    ok, msg = world_identity_verdict(_manifest(manifest_schema=99), _manifest())
    assert ok is False
    assert "manifest_schema=99" in msg


def test_world_identity_verdict_repeats_that_a_backfill_proves_nothing():
    """A manifest written into a world that already had tiles describes the run
    that wrote it and NOT those tiles. It must keep saying so."""
    from terrain_service.world_manifest import world_identity_verdict

    ok, msg = world_identity_verdict(
        _manifest(tiles_predate_manifest=True), _manifest()
    )
    assert ok is True
    assert "back-filled" in msg
    assert "cannot be recovered" in msg


# -- and the same policy through the real CLI --------------------------------


def _six_rasters(root):
    """Stand-in conditioning data: six files whose BYTES are what matter."""
    from terrain_service.providers.diffusion import DEFAULT_CONDITIONING_FILES

    root.mkdir(parents=True, exist_ok=True)
    for name in DEFAULT_CONDITIONING_FILES:
        (root / name).write_bytes(b"canonical:" + name.encode())
    return root


def _pregen(cache_dir, seed, *extra, env=None):
    import os

    return subprocess.run(
        ["python", "-m", "terrain_service.pregen",
         "--seed", str(seed), "--radius", "0",
         "--cache-dir", str(cache_dir), *extra],
        cwd=Path(__file__).parent.parent,
        capture_output=True, text=True,
        env=dict(os.environ, **(env or {})),
    )


def test_pregen_writes_the_identity_record_without_being_asked(tmp_path):
    """No flag. The world that can never be extended was lost by a bring-up
    that had no reason to think of this; a switch would have been off too."""
    import json

    from terrain_service.providers.synthetic import SyntheticProvider

    cache_dir = tmp_path / "cache"
    assert _pregen(cache_dir, 20260719, "--provider", "synthetic").returncode == 0

    path = (cache_dir / SyntheticProvider.provider_id / "000000000135276f"
            / MANIFEST_NAME)
    manifest = json.loads(path.read_text())
    assert manifest["manifest_schema"] == 1
    assert manifest["tiles_predate_manifest"] is False
    assert manifest["created_utc"].endswith("Z")
    assert manifest["identity"]["provider_id"] == SyntheticProvider.provider_id
    assert manifest["identity"]["seed"] == 20260719
    assert manifest["identity"]["seed_hex"] == "000000000135276f"


def test_pregen_records_every_identity_field_the_measurement_asked_for(tmp_path):
    """provider_id, seed, checkpoint sha256, conditioning digest, per-file
    sha256 of ALL SIX conditioning files, terrain-diffusion version and a UTC
    timestamp -- the triple whose absence made the 289-tile world final."""
    import json

    from terrain_service.providers.diffusion import DEFAULT_CONDITIONING_FILES

    cache_dir = tmp_path / "cache"
    env = {"TERRAIN_CONDITIONING_ROOT": str(_six_rasters(tmp_path / "global"))}
    run = _pregen(cache_dir, 7, "--provider", "diffusion", "--dry-run", env=env)
    assert run.returncode == 0, run.stderr

    ident = json.loads(
        next(cache_dir.glob("*/*/" + MANIFEST_NAME)).read_text()
    )["identity"]
    assert set(DEFAULT_CONDITIONING_FILES) == set(ident["conditioning_file_sha256"])
    assert ident["checkpoint_sha256"] == "UNPINNED"
    assert ident["conditioning_digest"] == "UNVERIFIED"
    assert ident["terrain_diffusion_version"]
    assert ident["namespace_id"] == ident["provider_id"]


def test_pregen_refuses_when_the_conditioning_bytes_moved_under_a_stable_id(tmp_path):
    """THE POD FAILURE, END TO END. provider_id is formed from the config's
    conditioning_digest, so an UNVERIFIED (or merely stale) digest leaves the
    namespace unchanged while the rasters underneath it change -- which is how
    one namespace comes to hold two planets. The manifest hashes the files
    themselves, so it sees what the namespace cannot."""
    root = _six_rasters(tmp_path / "global")
    cache_dir = tmp_path / "cache"
    env = {"TERRAIN_CONDITIONING_ROOT": str(root)}

    first = _pregen(cache_dir, 7, "--provider", "diffusion", "--dry-run", env=env)
    assert first.returncode == 0, first.stderr

    # What bootstrap_pod.sh did on the fresh pod: rebuilt etopo, got other bytes.
    (root / "etopo_10m.tif").write_bytes(b"rebuilt on a different machine")
    second = _pregen(cache_dir, 7, "--provider", "diffusion", "--dry-run", env=env)
    assert second.returncode == 1
    assert "etopo_10m.tif" in second.stderr
    assert "Nothing has been generated" in second.stderr
    # ...and nothing was: the refusal lands before the first tile.
    assert "generated=1" not in second.stderr


def test_pregen_second_run_into_the_same_world_is_silent(tmp_path):
    """Extending a world must not be noisy, or the noise stops being read."""
    cache_dir = tmp_path / "cache"
    env = {"TERRAIN_CONDITIONING_ROOT": str(_six_rasters(tmp_path / "global"))}
    assert _pregen(cache_dir, 7, "--provider", "diffusion", "--dry-run",
                   env=env).returncode == 0
    again = _pregen(cache_dir, 7, "--provider", "diffusion", "--dry-run", env=env)
    assert again.returncode == 0, again.stderr
    assert "warning" not in again.stderr and "error" not in again.stderr


def test_pregen_backfills_a_world_that_predates_the_record_and_says_so(tmp_path):
    """What the 289-tile world gets: a record from today, plus a warning that
    it says nothing about the tiles already there."""
    import json

    from terrain_service.providers.synthetic import SyntheticProvider

    cache_dir = tmp_path / "cache"
    assert _pregen(cache_dir, 42, "--provider", "synthetic").returncode == 0
    world = cache_dir / SyntheticProvider.provider_id / f"{42:016x}"
    (world / MANIFEST_NAME).unlink()  # a world generated before this existed

    again = _pregen(cache_dir, 42, "--provider", "synthetic")
    assert again.returncode == 0, again.stderr
    assert "already contained tiles but no identity record" in again.stderr
    assert json.loads((world / MANIFEST_NAME).read_text())["tiles_predate_manifest"]


def test_pregen_refuses_an_unreadable_identity_record(tmp_path):
    """Not by replacing it. A damaged provenance claim overwritten with a
    confident one is worse than the damage."""
    from terrain_service.providers.synthetic import SyntheticProvider

    cache_dir = tmp_path / "cache"
    assert _pregen(cache_dir, 42, "--provider", "synthetic").returncode == 0
    path = (cache_dir / SyntheticProvider.provider_id / f"{42:016x}"
            / MANIFEST_NAME)
    path.write_text("{ this is not json")

    again = _pregen(cache_dir, 42, "--provider", "synthetic")
    assert again.returncode == 1
    assert "unreadable" in again.stderr
    assert path.read_text() == "{ this is not json"


# -------------------------------------------------------------------------
# --codec. _encode_fine has accepted a codec since 2026-08-01 but no caller
# passed one, so every fine tile ever written by pregen is uncompressed.
# ---------------------------------------------------------------------------


def test_resolve_codec_unspecified_stays_raw_even_though_the_cli_says_zstd():
    """The FUNCTION's fallback and the CLI's default are deliberately different.

    The CLI defaults to zstd (production stores at 33 MB, not 190). This
    function must not: it is a library answering for a caller that expressed no
    preference, CI deliberately does not install ``zstandard``, and CODEC_RAW is
    the only answer that never needs an optional package. Keep them apart.
    """
    from terrain_service import tile_codec as tc
    from terrain_service.pregen import _resolve_codec

    assert _resolve_codec("raw") == tc.CODEC_RAW
    assert _resolve_codec("") == tc.CODEC_RAW
    assert _resolve_codec(None) == tc.CODEC_RAW


def test_cli_default_codec_is_zstd():
    """Guards the owner's 2026-08-03 decision to store fine tiles compressed.

    Asserted on the parser rather than by running a bake, so it holds on a box
    without ``zstandard`` -- where the run would (correctly) refuse.
    """
    from terrain_service.pregen import build_parser

    args = build_parser().parse_args(["--seed", "1", "--radius", "0"])
    assert args.codec == "zstd"

    # And the opt-out still works, because a box without zstandard needs one.
    assert build_parser().parse_args(
        ["--seed", "1", "--radius", "0", "--codec", "raw"]
    ).codec == "raw"


def test_codec_is_not_part_of_tile_identity():
    """A RAW tile and a ZSTD tile of the same ground are the SAME world.

    This is what makes the default switch safe for tiles already on disk: the
    identity payload carries no codec, so nothing already baked is orphaned and
    the client reads both. If a codec key ever appears here, every existing
    fine tile silently leaves its own namespace.
    """
    from terrain_service.bake.pipeline import bake_identity_payload

    assert not [k for k in bake_identity_payload() if "codec" in k.lower()]


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


# ---------------------------------------------------------------------------
# find_fine_namespaces -- READING BACK which namespace a client should be
# pointed at.
#
# On 2026-08-18 the fine-tier residency gate killed three capture runs, each
# worked around by hand-pinning -VoxelFineTileProviderId. Neither side had a
# wrong hash: the bake COMPUTES its namespace (fine_id_for, whose -bXXXXXXXX
# suffix moved -bdcab4bed -> -b19d281fd at bake_ver 27 -> 28) and the engine
# COMPUTES NOTHING (it reads a literal from the command line or
# DefaultGame.ini). The only reconciliation was a human copying nine hex
# characters into an ini. These pin the reader that replaces that human.
#
# Filesystem-only: no GPU, no checkpoint, no rasters -- the same reason
# world_identity_verdict is pure.
# ---------------------------------------------------------------------------


def _bake_namespace(root, namespace, seed_hex, tiles, created=None):
    """Lay out <root>/<namespace>/<seed_hex>/s16/<x>_<y>.vxtl on disk."""
    import json as _json

    world = root / namespace / seed_hex
    (world / "s16").mkdir(parents=True, exist_ok=True)
    for tx, ty in tiles:
        (world / "s16" / f"{tx}_{ty}.vxtl").write_bytes(b"not a real tile")
    if created is not None:
        (world / MANIFEST_NAME).write_text(
            _json.dumps({
                "manifest_schema": 1,
                "created_utc": created,
                "identity": {"namespace_id": namespace,
                             "provider_id": namespace.rsplit("-b", 1)[0]},
            }),
            encoding="utf-8",
        )
    return world


def test_find_fine_namespaces_finds_the_tile_under_the_other_id(tmp_path):
    """THE 2026-08-18 FAILURE, reproduced and answered.

    Two bakes of the same coarse world, the wanted tile in the NEWER one only.
    A client pinned to the older id gets a fatal residency gate; the reader has
    to name the namespace that actually holds the tile.
    """
    from terrain_service.world_manifest import find_fine_namespaces

    seed, seed_hex = 20260719, f"{20260719:016x}"
    coarse = "terrain-diffusion-unlabeled-80b9ca451a23eae4"
    _bake_namespace(tmp_path, f"{coarse}-bdcab4bed", seed_hex, [(-9, -9)],
                    created="2026-08-07T12:00:00Z")
    _bake_namespace(tmp_path, f"{coarse}-b19d281fd", seed_hex, [(-3, -4)],
                    created="2026-08-18T00:23:14Z")

    got = find_fine_namespaces(tmp_path, seed, tiles=[(-3, -4)])
    holders = [g["namespace_id"] for g in got if not g["tiles_missing"]]
    assert holders == [f"{coarse}-b19d281fd"]
    # And the stale pin is reported as present-but-short, not as absent: the
    # operator needs to see that the id they passed IS a real namespace here,
    # or they will go looking for a broken cache root instead of a stale pin.
    stale = next(g for g in got if g["namespace_id"] == f"{coarse}-bdcab4bed")
    assert stale["tiles_missing"] == [(-3, -4)]


def test_find_fine_namespaces_orders_newest_first(tmp_path):
    """--newest has to mean newest. Ordering is the only thing standing between
    a capture and silently measuring last week's bake.
    """
    from terrain_service.world_manifest import find_fine_namespaces

    seed, seed_hex = 20260719, f"{20260719:016x}"
    coarse = "terrain-diffusion-unlabeled-80b9ca451a23eae4"
    for suffix, created in (("-b111111a", "2026-08-04T02:10:07Z"),
                            ("-b222222b", "2026-08-18T00:23:14Z"),
                            ("-b333333c", "2026-08-05T13:36:52Z")):
        _bake_namespace(tmp_path, coarse + suffix, seed_hex, [(-3, -4)],
                        created=created)
    # No manifest at all: dateless, and must sort LAST rather than first --
    # "nobody can date it" is not "it is from the beginning of time".
    _bake_namespace(tmp_path, coarse + "-b444444d", seed_hex, [(-3, -4)])

    got = find_fine_namespaces(tmp_path, seed, tiles=[(-3, -4)])
    assert [g["namespace_id"][-9:] for g in got] == [
        "-b222222b", "-b333333c", "-b111111a", "-b444444d"]
    assert got[-1]["has_manifest"] is False


def test_find_fine_namespaces_ignores_other_seeds_and_other_worlds(tmp_path):
    """A namespace holding nothing for THIS seed is not a candidate, and
    --provider-id keeps two coarse worlds' fine bakes apart. Prefix matching is
    sound only because fine_id_for SUFFIXES rather than re-hashes.
    """
    from terrain_service.world_manifest import find_fine_namespaces

    seed, seed_hex = 20260719, f"{20260719:016x}"
    other_hex = f"{99999999:016x}"
    mine = "terrain-diffusion-unlabeled-80b9ca451a23eae4"
    theirs = "terrain-diffusion-unlabeled-71e2b362e3241e71"
    _bake_namespace(tmp_path, f"{mine}-b19d281fd", seed_hex, [(-3, -4)],
                    created="2026-08-18T00:23:14Z")
    _bake_namespace(tmp_path, f"{theirs}-b19d281fd", seed_hex, [(-3, -4)],
                    created="2026-08-18T00:23:14Z")
    _bake_namespace(tmp_path, f"{mine}-bfffffff", other_hex, [(-3, -4)],
                    created="2026-08-18T00:23:14Z")

    both = find_fine_namespaces(tmp_path, seed, tiles=[(-3, -4)])
    assert len(both) == 2  # the other SEED is not a candidate at all

    only_mine = find_fine_namespaces(tmp_path, seed, tiles=[(-3, -4)],
                                     coarse_provider_id=mine)
    assert [g["namespace_id"] for g in only_mine] == [f"{mine}-b19d281fd"]


def test_find_fine_namespaces_reports_a_moved_record(tmp_path):
    """A record naming a namespace other than the directory holding it has been
    copied by hand. The DIRECTORY is what the client opens, so it is what gets
    reported -- but the disagreement must be visible, since hand cache surgery
    is how one namespace ends up holding two planets.
    """
    import json as _json

    from terrain_service.world_manifest import find_fine_namespaces

    seed, seed_hex = 20260719, f"{20260719:016x}"
    world = _bake_namespace(tmp_path, "ns-b19d281fd", seed_hex, [(-3, -4)],
                            created="2026-08-18T00:23:14Z")
    rec = _json.loads((world / MANIFEST_NAME).read_text(encoding="utf-8"))
    rec["identity"]["namespace_id"] = "ns-bdcab4bed"  # copied from elsewhere
    (world / MANIFEST_NAME).write_text(_json.dumps(rec), encoding="utf-8")

    got = find_fine_namespaces(tmp_path, seed, tiles=[(-3, -4)])
    assert got[0]["namespace_id"] == "ns-b19d281fd"           # the directory
    assert got[0]["recorded_namespace_id"] == "ns-bdcab4bed"  # the claim


def test_find_fine_namespaces_survives_an_unreadable_record(tmp_path):
    """A corrupt manifest must not hide the tiles beside it. The 289-tile world
    has no manifest at all; a reader that answered "nowhere" for the namespaces
    it cannot vouch for would be useless for the world that matters most.
    """
    from terrain_service.world_manifest import find_fine_namespaces

    seed, seed_hex = 20260719, f"{20260719:016x}"
    world = _bake_namespace(tmp_path, "ns-b19d281fd", seed_hex, [(-3, -4)])
    (world / MANIFEST_NAME).write_bytes(b"\xff\xfe not json at all")

    got = find_fine_namespaces(tmp_path, seed, tiles=[(-3, -4)])
    assert [g["namespace_id"] for g in got] == ["ns-b19d281fd"]
    assert got[0]["has_manifest"] is False
    assert got[0]["tiles_missing"] == []


def test_find_fine_namespaces_on_a_missing_root_is_empty_not_an_error(tmp_path):
    """A wrong --cache-dir must produce "nothing here", which the tool turns
    into a refusal. Raising instead would tempt a harness into an except: that
    falls back to a default id -- and a wrong default id is the fatal gate.
    """
    from terrain_service.world_manifest import find_fine_namespaces

    assert find_fine_namespaces(tmp_path / "nope", 20260719) == []
