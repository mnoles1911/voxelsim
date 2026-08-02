"""Diffusion provider tests (no GPU): validate_model_output, the dry-run
config->adapter->validate->encode pipeline, provider_id stability, checkpoint
sha256 verification, per-tile seed derivation, and DiffusionProvider's
model_backend injection point. Everything here MUST pass without a CUDA
machine — TerrainDiffusionBackend's real inference call (the only piece that
needs one) is exercised only indirectly, via a fake backend standing in for
it, per DiffusionProvider(model_backend=...)."""

from __future__ import annotations

import numpy as np
import pytest

from terrain_service import tile_codec
from terrain_service.app import _make_provider, create_app
from terrain_service.cache import TileCache
from terrain_service.providers.diffusion import (
    CLIMATE_CALIBRATION,
    DEFAULT_CHANNEL_MAPPING,
    DEFAULT_CONDITIONING_FILES,
    EXPECTED_CHANNELS,
    UNVERIFIED,
    ConditioningDataMissing,
    DiffusionConfig,
    DiffusionProvider,
    ModelOutputMismatch,
    WorldShapeConfig,
    TerrainDiffusionBackend,
    adapt_raster_to_tile,
    apply_climate_calibration,
    clamp_to_physical_range,
    compute_conditioning_digest,
    derive_tile_seed,
    resolve_conditioning_root,
    validate_model_output,
    verify_checkpoint_sha256,
    verify_conditioning_digest,
)

from terrain_service.providers.synthetic import SyntheticProvider

TILE_SIZE = tile_codec.TILE_SIZE

#: Dry-run provider_id suffix. It carries SyntheticProvider's own version
#: because in dry-run mode the STAND-IN, not the checkpoint, decides the
#: bytes -- bumping "synthetic-v1" must roll the dry-run cache namespace.
DRYRUN = "-dryrun-" + SyntheticProvider.provider_id


def _good_raster(mapping: dict[str, str] | None = None) -> dict[str, np.ndarray]:
    mapping = mapping or DEFAULT_CHANNEL_MAPPING
    # Climate values are RAW WorldClim physical units (bio_1/4/12/15), not
    # [0, 1] -- confirmed against a real checkpoint 2026-07-22. These are the
    # exact midpoint of each channel's declared range, so every one quantizes
    # to 128 and a normalization bug cannot hide behind a coincidence.
    raster = {mapping["elevation"]: np.full((TILE_SIZE, TILE_SIZE), 100.0, dtype=np.float32)}
    midpoints = {
        "temperature": 0.0,  # degrees C, range [-40, 40]
        "seasonality": 1500.0,  # sd x 100, range [0, 3000]
        "precipitation": 6000.0,  # mm/yr, range [0, 12000]
        "precip_variability": 100.0,  # CV %, range [0, 200]
    }
    for name, value in midpoints.items():
        raster[mapping[name]] = np.full((TILE_SIZE, TILE_SIZE), value, dtype=np.float32)
    return raster


# ---------------------------------------------------------------------------
# validate_model_output
# ---------------------------------------------------------------------------


def test_validate_model_output_accepts_correct_manifest():
    validate_model_output(_good_raster())  # must not raise


def test_validate_model_output_rejects_wrong_channel_count():
    raster = _good_raster()
    del raster["seasonality"]
    with pytest.raises(ModelOutputMismatch) as exc:
        validate_model_output(raster)
    msg = str(exc.value)
    assert "seasonality" in msg
    assert "channel count mismatch" in msg
    assert "expected 5" in msg and "has 4" in msg


def test_validate_model_output_rejects_extra_channel():
    raster = _good_raster()
    raster["humidity"] = np.zeros((TILE_SIZE, TILE_SIZE), dtype=np.float32)
    with pytest.raises(ModelOutputMismatch) as exc:
        validate_model_output(raster)
    assert "humidity" in str(exc.value)
    assert "extra channel" in str(exc.value)


def test_validate_model_output_rejects_wrong_dtype():
    raster = _good_raster()
    raster["elevation"] = raster["elevation"].astype(np.float64)
    with pytest.raises(ModelOutputMismatch) as exc:
        validate_model_output(raster)
    msg = str(exc.value)
    assert "elevation" in msg and "dtype" in msg
    assert "float64" in msg and "float32" in msg


def test_validate_model_output_rejects_out_of_range_values():
    raster = _good_raster()
    # 50,000 mm/yr is ~4x the wettest place on Earth, so it is out of range
    # under the physical bio_12 bounds. (This used to be 5.0, which only
    # worked while we wrongly believed climate arrived normalized to [0, 1].)
    raster["precipitation"] = np.full((TILE_SIZE, TILE_SIZE), 50000.0, dtype=np.float32)
    with pytest.raises(ModelOutputMismatch) as exc:
        validate_model_output(raster)
    msg = str(exc.value)
    assert "precipitation" in msg and "range" in msg


def test_validate_model_output_reports_all_issues_at_once():
    """Bring-up sessions want the whole mismatch picture in one run, not
    one issue per re-run."""
    raster = _good_raster()
    raster["elevation"] = raster["elevation"].astype(np.float64)  # dtype issue
    # 50,000 mm/yr is ~4x the wettest place on Earth, so it is out of range
    # under the physical bio_12 bounds. (This used to be 5.0, which only
    # worked while we wrongly believed climate arrived normalized to [0, 1].)
    raster["precipitation"] = np.full((TILE_SIZE, TILE_SIZE), 50000.0, dtype=np.float32)  # range issue
    del raster["seasonality"]  # missing-channel issue
    with pytest.raises(ModelOutputMismatch) as exc:
        validate_model_output(raster)
    assert len(exc.value.issues) >= 3


def test_validate_model_output_respects_custom_channel_mapping():
    """If the real checkpoint names channels differently, that's a config
    edit (channel_mapping), not a code change."""
    custom_mapping = dict(DEFAULT_CHANNEL_MAPPING)
    custom_mapping["precip_variability"] = "precip_var"
    raster = _good_raster(custom_mapping)
    validate_model_output(raster, custom_mapping)  # must not raise

    # Using the DEFAULT mapping against this custom-keyed raster should now
    # fail (key "precip_variability" is absent, "precip_var" is unexpected).
    with pytest.raises(ModelOutputMismatch):
        validate_model_output(raster)


# ---------------------------------------------------------------------------
# adapt_raster_to_tile
# ---------------------------------------------------------------------------


def test_adapt_raster_to_tile_quantizes_correctly():
    # climate_calibration=False on purpose: this test pins the QUANTIZER, and
    # the hand-chosen physical values below are meaningful only if they reach
    # it unmodified. The calibration is a separate transform with its own
    # tests; leaving it on here would silently turn this into a test of both
    # and make the expected constants unreadable.
    config = DiffusionConfig(climate_calibration=False)
    raster = _good_raster()
    raster["elevation"] = np.full((TILE_SIZE, TILE_SIZE), 1234.6, dtype=np.float32)
    raster["temperature"] = np.full((TILE_SIZE, TILE_SIZE), 40.0, dtype=np.float32)
    tile = adapt_raster_to_tile(raster, config, seed=1, x=0, y=0, scale=1)
    assert tile.elevation.dtype == np.int16
    assert int(tile.elevation[0, 0]) == 1235  # rounds
    assert tile.climate.dtype == np.uint8
    # +40 C is the top of temperature's physical range -> saturates to 255.
    assert tile.climate[0, 0, 0] == 255
    # The other three are at their range midpoints (see _good_raster) -> 128.
    # Under the old `raw * 255` quantization these all clipped to 255, which
    # is exactly the bug this asserts against.
    assert tile.climate[1, 0, 0] == 128  # seasonality 1500 of [0, 3000]
    assert tile.climate[2, 0, 0] == 128  # precipitation 6000 of [0, 12000]
    assert tile.climate[3, 0, 0] == 128  # precip_variability 100 of [0, 200]


def test_climate_calibration_curves_are_strictly_monotone():
    """The whole safety argument rests on this.

    A monotone map cannot move a warm cell somewhere else -- it only relabels
    how warm it is -- which is why calibrating model output leaves spatial
    structure exactly intact. If a curve ever became non-monotone, two
    different physical values would swap order and the terrain/climate
    correlation the orographic term builds would be silently scrambled.
    """
    for name, (xs, ys) in CLIMATE_CALIBRATION.items():
        assert len(xs) == len(ys) >= 2, name
        assert all(b > a for a, b in zip(xs, xs[1:])), f"{name}: xs not increasing"
        assert all(b > a for a, b in zip(ys, ys[1:])), f"{name}: ys not increasing"


def test_climate_calibration_extrapolates_rather_than_clamping():
    """np.interp CLAMPS outside its anchors; this must not.

    Clamping would pin every value above the top anchor to one output -- a
    hard ceiling exactly where the hot, arid, desert-forming cells live, which
    is the range compression the calibration exists to undo. Extending the end
    segments' slopes keeps the map monotone over the whole real line.
    """
    xs, ys = CLIMATE_CALIBRATION["temperature"]
    beyond = np.array([xs[-1], xs[-1] + 1.0, xs[-1] + 5.0], dtype=np.float64)
    out = apply_climate_calibration(beyond, xs, ys)
    assert out[1] > out[0], "top end clamped instead of extrapolating"
    assert out[2] > out[1]
    # Slope beyond the last anchor must match the last segment's.
    expected = (ys[-1] - ys[-2]) / (xs[-1] - xs[-2])
    assert out[2] - out[1] == pytest.approx(expected * 4.0, rel=1e-9)

    below = np.array([xs[0], xs[0] - 1.0, xs[0] - 5.0], dtype=np.float64)
    lo = apply_climate_calibration(below, xs, ys)
    assert lo[1] < lo[0], "bottom end clamped instead of extrapolating"
    assert lo[2] < lo[1]


def test_climate_calibration_restores_the_hot_arid_corner():
    """The measured failure it was built for.

    The model delivered land temperature capped near 20.5 C p95 on the coarse
    stage, so cells that were simultaneously hot (>=24 C) and arid were 0.00%
    of land everywhere, and no biome threshold could produce an honest desert.

    20.512 is that ORIGINAL coarse-stage p95, kept deliberately as a fixed
    regression point rather than tracking whichever percentile the current
    curves were fitted at -- the curves are now fitted on the full pipeline,
    where raw p95 is 25.9, but this value is what the bug looked like and any
    calibration worth shipping must still lift it past the desert gate.
    """
    xs, ys = CLIMATE_CALIBRATION["temperature"]
    delivered_p95 = np.array([20.512], dtype=np.float64)
    assert apply_climate_calibration(delivered_p95, xs, ys)[0] >= 24.0


def test_climate_calibration_is_in_the_identity():
    """Re-fitting the curves changes every tile's climate; it must roll the id.

    Same lesson as synthetic_map_stats.json: a derived table that decides the
    world is not allowed to change under an unchanged provider_id.
    """
    on = DiffusionConfig(climate_calibration=True).provider_id()
    off = DiffusionConfig(climate_calibration=False).provider_id()
    assert on != off


def test_adapt_raster_to_tile_clips_out_of_range_elevation():
    config = DiffusionConfig()
    raster = _good_raster()
    raster["elevation"] = np.full((TILE_SIZE, TILE_SIZE), 1e9, dtype=np.float32)
    tile = adapt_raster_to_tile(raster, config, seed=1, x=0, y=0, scale=1)
    assert int(tile.elevation[0, 0]) == 32767


# ---------------------------------------------------------------------------
# DiffusionConfig.provider_id
# ---------------------------------------------------------------------------


def test_provider_id_stable_and_deterministic():
    a = DiffusionConfig().provider_id()
    b = DiffusionConfig().provider_id()
    assert a == b
    assert a.startswith("terrain-diffusion-")


def test_provider_id_changes_with_config():
    """Every config field that can change generated BYTES must roll the id.

    ``checkpoint_id`` is deliberately absent from this list — it is a load
    LOCATION, and it has its own test below asserting it does NOT roll the
    id. That is the point of the fix, not an omission.
    """
    base = DiffusionConfig().provider_id()
    diff_label = DiffusionConfig(checkpoint_label="other-label").provider_id()
    diff_hash = DiffusionConfig(checkpoint_sha256="deadbeef").provider_id()
    diff_conditioning = DiffusionConfig(conditioning_digest="c" * 64).provider_id()
    diff_cond_files = DiffusionConfig(
        conditioning_files=DEFAULT_CONDITIONING_FILES + ("wc2.1_10m_bio_7.tif",)
    ).provider_id()
    diff_td_version = DiffusionConfig(terrain_diffusion_version="1.2.3").provider_id()
    # Every world-shape kwarg must roll the id -- these are the ones that
    # actually reach WorldPipeline, unlike the SamplerConfig they replaced.
    diff_freq = DiffusionConfig(
        world_shape=WorldShapeConfig(frequency_mult=(0.4, 3.0, 3.0, 3.0, 3.0))
    ).provider_id()
    diff_water = DiffusionConfig(
        world_shape=WorldShapeConfig(drop_water_pct=0.25)
    ).provider_id()
    diff_snr = DiffusionConfig(
        world_shape=WorldShapeConfig(cond_snr=(0.9, 0.1, 1.0, 0.1, 1.0))
    ).provider_id()
    diff_pool = DiffusionConfig(
        world_shape=WorldShapeConfig(coarse_pooling=2)
    ).provider_id()
    diff_poolmode = DiffusionConfig(
        world_shape=WorldShapeConfig(elev_coarse_pool_mode='max')
    ).provider_id()
    diff_scale = DiffusionConfig(scale=8).provider_id()
    diff_mapping = DiffusionConfig(
        channel_mapping={**DEFAULT_CHANNEL_MAPPING, "precip_variability": "precip_var"}
    ).provider_id()
    ids = {
        base,
        diff_label,
        diff_hash,
        diff_conditioning,
        diff_cond_files,
        diff_td_version,
        diff_freq,
        diff_water,
        diff_snr,
        diff_pool,
        diff_poolmode,
        diff_scale,
        diff_mapping,
    }
    assert len(ids) == 13, "every byte-affecting config field must roll the provider_id"


def test_provider_id_ignores_checkpoint_load_path():
    """BUG 1. The same checkpoint mounted at two paths is the SAME checkpoint.

    Observed on the pod: `terrain-diffusion-/workspace/ckpt/terrain-diffusion-
    30m-<digest>` — the local filesystem path was embedded in the identity, so
    the identical checkpoint mounted elsewhere produced a different
    provider_id, and EditLog::checkProvider() reported kMismatch (refuse the
    replay outright) on a world that was byte-for-byte fine.
    """
    pod = DiffusionConfig(
        checkpoint_id="/workspace/ckpt/terrain-diffusion-30m",
        checkpoint_label="terrain-diffusion-30m",
        checkpoint_sha256="a" * 64,
        conditioning_digest="b" * 64,
    )
    laptop = DiffusionConfig(
        checkpoint_id=r"D:\ckpt\td30m",
        checkpoint_label="terrain-diffusion-30m",
        checkpoint_sha256="a" * 64,
        conditioning_digest="b" * 64,
    )
    assert pod.provider_id() == laptop.provider_id()
    # ...and no path fragment leaks into the id at all.
    assert "workspace" not in pod.provider_id()
    assert "/" not in pod.provider_id() and "\\" not in pod.provider_id()


def test_checkpoint_label_rejects_a_path():
    """The old habit (put the mount point in the identity field) must fail
    loudly rather than silently reinstate bug 1."""
    for bad in ("/workspace/ckpt/td30m", r"D:\ckpt", "a:b", "", " padded"):
        with pytest.raises(ValueError, match="checkpoint_label"):
            DiffusionConfig(checkpoint_label=bad)


def test_unpinned_and_unverified_configs_are_visibly_marked():
    """An id that cannot be trusted must not LOOK like a normal one, so a
    stray cache dir or edit-log stamp is self-describing."""
    default = DiffusionConfig().provider_id()
    assert "UNPINNED" in default and "UNVERIFIEDDATA" in default

    ckpt_only = DiffusionConfig(checkpoint_sha256="a" * 64).provider_id()
    assert "UNPINNED" not in ckpt_only and "UNVERIFIEDDATA" in ckpt_only

    fully_pinned = DiffusionConfig(
        checkpoint_sha256="a" * 64, conditioning_digest="b" * 64
    ).provider_id()
    assert "UNPINNED" not in fully_pinned and "UNVERIFIED" not in fully_pinned


def test_provider_id_override_adopts_an_existing_namespace_verbatim():
    """The documented migration escape hatch: adopt a cache namespace that
    already exists on disk (e.g. tiles generated under the v1 id)."""
    legacy = "terrain-diffusion-/workspace/ckpt/terrain-diffusion-30m-c6f775d3f1b21289"
    config = DiffusionConfig(provider_id_override=legacy)
    assert config.provider_id() == legacy
    assert DiffusionProvider(config=config, dry_run=False).provider_id == legacy


# ---------------------------------------------------------------------------
# BUG 2: conditioning data (WorldClim bio rasters + data/global/etopo_10m.tif)
# ---------------------------------------------------------------------------


def _write_conditioning(root, etopo_bytes: bytes = b"etopo-bed"):
    root.mkdir(parents=True, exist_ok=True)
    for name in DEFAULT_CONDITIONING_FILES:
        (root / name).write_bytes(
            etopo_bytes if name == "etopo_10m.tif" else name.encode()
        )
    return root


def test_conditioning_digest_hashes_content_not_metadata(tmp_path):
    """Two boxes whose ETOPO differs (fetch_etopo.py's candidate list spans a
    '_bed' and a '_surface' NOAA variant) must produce different digests —
    and identical content must produce the same digest regardless of where it
    sits or when it was downloaded."""
    bed = _write_conditioning(tmp_path / "bed")
    surface = _write_conditioning(tmp_path / "surface", etopo_bytes=b"etopo-surface!")
    copy = _write_conditioning(tmp_path / "elsewhere")

    d_bed = compute_conditioning_digest(root=bed)
    d_surface = compute_conditioning_digest(root=surface)
    d_copy = compute_conditioning_digest(root=copy)

    assert d_bed != d_surface, "different ETOPO bytes must be a different identity"
    assert d_bed == d_copy, "same bytes at a different path must be the same identity"
    assert len(d_bed) == 64


def test_conditioning_digest_ignores_unlisted_files(tmp_path):
    """The raw multi-hundred-MB NOAA intermediate (_etopo_source.tif) and any
    extra bio rasters a box happened to download are NOT generation inputs;
    hashing them would cause false kMismatch — bug 1's failure mode again."""
    root = _write_conditioning(tmp_path / "d")
    before = compute_conditioning_digest(root=root)
    (root / "_etopo_source.tif").write_bytes(b"huge raw download")
    (root / "wc2.1_10m_bio_9.tif").write_bytes(b"unused")
    assert compute_conditioning_digest(root=root) == before


def test_conditioning_digest_refuses_when_data_absent(tmp_path):
    """A provider that cannot see its conditioning data refuses to claim an
    identity rather than inventing one."""
    root = _write_conditioning(tmp_path / "d")
    (root / "etopo_10m.tif").unlink()
    with pytest.raises(ConditioningDataMissing) as exc:
        compute_conditioning_digest(root=root)
    assert "etopo_10m.tif" in str(exc.value)


def test_verify_conditioning_digest_refuses_unverified_and_mismatch(tmp_path):
    root = _write_conditioning(tmp_path / "d")
    actual = compute_conditioning_digest(root=root)

    assert DiffusionConfig().conditioning_digest == UNVERIFIED
    with pytest.raises(ValueError, match="UNVERIFIED"):
        verify_conditioning_digest(DiffusionConfig(), root=root)

    with pytest.raises(ValueError, match="conditioning data mismatch"):
        verify_conditioning_digest(
            DiffusionConfig(conditioning_digest="f" * 64), root=root
        )

    # Matching data verifies silently.
    verify_conditioning_digest(DiffusionConfig(conditioning_digest=actual), root=root)


def test_conditioning_root_resolution_is_absolute_and_overridable(tmp_path, monkeypatch):
    """The upstream path is RELATIVE (resolved from CWD by
    synthetic_map._compute_map_stats), so resolution is explicit here — but
    the resolved absolute path is used only to open files, never hashed."""
    monkeypatch.delenv("TERRAIN_CONDITIONING_ROOT", raising=False)
    monkeypatch.chdir(tmp_path)
    assert resolve_conditioning_root() == (tmp_path / "data" / "global").resolve()

    monkeypatch.setenv("TERRAIN_CONDITIONING_ROOT", str(tmp_path / "envdir"))
    assert resolve_conditioning_root() == (tmp_path / "envdir").resolve()
    # An explicit argument beats the env var.
    assert resolve_conditioning_root(tmp_path / "argdir") == (tmp_path / "argdir").resolve()


def test_conditioning_digest_is_load_bearing_in_provider_id(tmp_path):
    """End to end: two boxes with different ETOPO builds, everything else
    identical, must land in different cache namespaces and stamp different
    edit logs — the divergence EditLog::checkProvider() previously could not
    see."""
    bed = _write_conditioning(tmp_path / "bed")
    surface = _write_conditioning(tmp_path / "surface", etopo_bytes=b"etopo-surface!")
    common = dict(
        checkpoint_id="/wherever",
        checkpoint_label="terrain-diffusion-30m",
        checkpoint_sha256="a" * 64,
    )
    id_bed = DiffusionConfig(
        conditioning_digest=compute_conditioning_digest(root=bed), **common
    ).provider_id()
    id_surface = DiffusionConfig(
        conditioning_digest=compute_conditioning_digest(root=surface), **common
    ).provider_id()
    assert id_bed != id_surface


# ---------------------------------------------------------------------------
# GAP 3: tile wire format / quantization constants live OUTSIDE DiffusionConfig
# ---------------------------------------------------------------------------


def test_provider_id_covers_quantization_range(monkeypatch):
    """EXPECTED_CHANNELS' min/max are the adapter's uint8 quantization range
    (see adapt_raster_to_tile), i.e. part of the tile wire format — editing
    one changes tile BYTES. They are module constants, not config fields, so
    before _tile_format_fingerprint() they could change under an unchanged
    provider_id."""
    from terrain_service.providers import diffusion as diff_mod

    before = DiffusionConfig().provider_id()
    patched = tuple(
        diff_mod.ChannelSpec(c.name, c.dtype, c.min, c.max * 2)
        if c.name == "precipitation"
        else c
        for c in EXPECTED_CHANNELS
    )
    monkeypatch.setattr(diff_mod, "EXPECTED_CHANNELS", patched)
    assert DiffusionConfig().provider_id() != before


def test_config_rejects_incomplete_channel_mapping():
    with pytest.raises(ValueError):
        DiffusionConfig(channel_mapping={"elevation": "elevation"})


def test_config_rejects_unsupported_scale():
    with pytest.raises(ValueError):
        DiffusionConfig(scale=3)


# ---------------------------------------------------------------------------
# DiffusionProvider dry-run end-to-end.
# ---------------------------------------------------------------------------


def test_dry_run_provider_produces_valid_encodable_tile():
    provider = DiffusionProvider(dry_run=True)
    tile = provider.generate(seed=42, x=1, y=-2, scale=1)
    encoded = tile_codec.encode(tile)
    decoded = tile_codec.decode(encoded)
    assert decoded.seed == 42 and decoded.x == 1 and decoded.y == -2
    np.testing.assert_array_equal(decoded.elevation, tile.elevation)
    np.testing.assert_array_equal(decoded.climate, tile.climate)


def test_dry_run_provider_id_is_tagged_and_distinct_from_real():
    dry = DiffusionProvider(dry_run=True)
    assert dry.provider_id.endswith(DRYRUN)

    # Real (non-dry-run) construction never touches a GPU — only generate()
    # does — so this must succeed without raising.
    real = DiffusionProvider(dry_run=False)
    assert not real.provider_id.endswith(DRYRUN)
    assert real.provider_id != dry.provider_id


def test_dry_run_provider_deterministic():
    provider = DiffusionProvider(dry_run=True)
    a = provider.generate(7, 3, 3, 1)
    b = provider.generate(7, 3, 3, 1)
    np.testing.assert_array_equal(a.elevation, b.elevation)
    np.testing.assert_array_equal(a.climate, b.climate)


def test_non_dry_run_without_backend_raises_clear_error_without_torch():
    """_call_model's real (non-dry-run, no injected backend) path now
    lazily constructs TerrainDiffusionBackend instead of raising
    NotImplementedError -- but on this dev machine (no torch/CUDA), that
    construction still fails, just with a clear, actionable RuntimeError
    (pointing at dry_run=True / model_backend injection, and the bring-up
    doc) instead of a raw ModuleNotFoundError traceback."""
    provider = DiffusionProvider(dry_run=False)
    with pytest.raises(RuntimeError, match="dry_run=True"):
        provider.generate(1, 0, 0, 1)


def test_provider_rejects_scale_mismatch():
    provider = DiffusionProvider(config=DiffusionConfig(scale=1), dry_run=True)
    with pytest.raises(ValueError, match="scale"):
        provider.generate(1, 0, 0, 8)


# ---------------------------------------------------------------------------
# Wiring: app._make_provider / TileCache / Flask route in dry-run mode.
# ---------------------------------------------------------------------------


def test_make_provider_diffusion_dry_run():
    provider = _make_provider("diffusion", dry_run=True)
    assert isinstance(provider, DiffusionProvider)
    assert provider.dry_run is True


def test_make_provider_diffusion_without_config_uses_unpinned_default():
    """Documents the PRE-existing (still valid) behavior: no config given ->
    DiffusionProvider's own UNPINNED default. Contrast with the next test,
    where a caller-supplied pinned config is NOT silently discarded."""
    provider = _make_provider("diffusion", dry_run=True)
    assert provider.config.checkpoint_sha256 == "UNPINNED"


def test_make_provider_diffusion_propagates_pinned_config():
    """The gap docs/pod-bringup-commands.md Block 5 flags: a caller-built
    pinned DiffusionConfig must actually reach DiffusionProvider, not be
    dropped in favor of the UNPINNED default. This is the core of the
    `_make_provider` fix."""
    pinned = DiffusionConfig(checkpoint_id="ckpt-x", checkpoint_sha256="a" * 64)
    provider = _make_provider("diffusion", dry_run=True, config=pinned)
    assert provider.config is pinned
    assert provider.config.checkpoint_sha256 == "a" * 64
    assert provider.provider_id.startswith(pinned.provider_id())


def test_make_provider_synthetic_ignores_config_arg():
    # config is diffusion-only; passing one for "synthetic" must not error
    # or affect the returned provider (kwarg is simply unused).
    from terrain_service.providers.synthetic import SyntheticProvider

    provider = _make_provider("synthetic", config=DiffusionConfig())
    assert isinstance(provider, SyntheticProvider)


def test_create_app_pins_diffusion_config_from_env(tmp_path, monkeypatch):
    """create_app (no explicit `provider=` override) must build its
    DiffusionProvider from TERRAIN_DIFFUSION_CHECKPOINT_ID/_SHA256, not
    silently fall back to the UNPINNED default -- same gap as the pregen
    CLI, same fix (_make_provider's config passthrough), same env
    surface the Flask server actually runs under."""
    monkeypatch.setenv("TERRAIN_PROVIDER", "diffusion")
    monkeypatch.setenv("TERRAIN_DIFFUSION_DRY_RUN", "1")
    monkeypatch.setenv("TERRAIN_DIFFUSION_CHECKPOINT_ID", "ckpt-env")
    monkeypatch.setenv("TERRAIN_DIFFUSION_CHECKPOINT_SHA256", "b" * 64)

    app = create_app(cache=TileCache(tmp_path))
    client = app.test_client()

    expected = (
        DiffusionConfig(checkpoint_id="ckpt-env", checkpoint_sha256="b" * 64).provider_id()
        + DRYRUN
    )
    assert client.get("/healthz").get_json()["provider"] == expected


def test_create_app_without_checkpoint_env_uses_unpinned_default(tmp_path, monkeypatch):
    monkeypatch.setenv("TERRAIN_PROVIDER", "diffusion")
    monkeypatch.setenv("TERRAIN_DIFFUSION_DRY_RUN", "1")
    monkeypatch.delenv("TERRAIN_DIFFUSION_CHECKPOINT_ID", raising=False)
    monkeypatch.delenv("TERRAIN_DIFFUSION_CHECKPOINT_SHA256", raising=False)

    app = create_app(cache=TileCache(tmp_path))
    client = app.test_client()

    expected = DiffusionConfig().provider_id() + DRYRUN
    assert client.get("/healthz").get_json()["provider"] == expected


def test_tile_api_diffusion_dry_run(tmp_path):
    provider = DiffusionProvider(dry_run=True)
    app = create_app(provider=provider, cache=TileCache(tmp_path))
    client = app.test_client()

    r = client.get("/tile?seed=1&x=0&y=0&scale=1")
    assert r.status_code == 200
    tile = tile_codec.decode(r.data)
    assert tile.elevation.shape == (TILE_SIZE, TILE_SIZE)

    r2 = client.get("/tile?seed=1&x=0&y=0&scale=1")
    assert r2.data == r.data  # cache-served, byte-identical

    assert client.get("/healthz").get_json()["provider"] == provider.provider_id


# ---------------------------------------------------------------------------
# DiffusionProvider(model_backend=...) injection -- how the real inference
# path is unit-tested without a GPU: a fake backend stands in for
# TerrainDiffusionBackend and must be driven through the exact same
# generate() -> _call_model -> validate -> adapt path a real backend would.
# ---------------------------------------------------------------------------


class _FakeBackend:
    """Crafted deterministic rasters, keyed by DEFAULT_CHANNEL_MAPPING, so
    tests can assert exact elevation/climate conversion through generate()."""

    def __init__(self):
        self.calls: list[tuple[int, int, int, int]] = []

    def generate_rasters(self, seed: int, x: int, y: int, scale: int) -> dict[str, np.ndarray]:
        self.calls.append((seed, x, y, scale))
        raster = {
            # Physical WorldClim units, chosen to land on distinct, exactly
            # representable quantization results (see EXPECTED_CHANNELS).
            "elevation": np.full((TILE_SIZE, TILE_SIZE), 1234.6, dtype=np.float32),
            "temperature": np.full((TILE_SIZE, TILE_SIZE), 40.0, dtype=np.float32),
            "seasonality": np.full((TILE_SIZE, TILE_SIZE), 0.0, dtype=np.float32),
            "precipitation": np.full((TILE_SIZE, TILE_SIZE), 6000.0, dtype=np.float32),
            "precip_variability": np.full((TILE_SIZE, TILE_SIZE), 50.0, dtype=np.float32),
        }
        return raster


class _BadShapeBackend:
    def generate_rasters(self, seed, x, y, scale):
        raster = _good_raster()
        raster["elevation"] = raster["elevation"][:, :256]  # non-square
        return raster


class _BadDtypeBackend:
    def generate_rasters(self, seed, x, y, scale):
        raster = _good_raster()
        raster["elevation"] = raster["elevation"].astype(np.float64)
        return raster


class _ExtraChannelBackend:
    def generate_rasters(self, seed, x, y, scale):
        raster = _good_raster()
        raster["humidity"] = np.zeros((TILE_SIZE, TILE_SIZE), dtype=np.float32)
        return raster


def test_injected_backend_produces_correct_tile():
    fake = _FakeBackend()
    # Uncalibrated, same reason as test_adapt_raster_to_tile_quantizes_correctly:
    # this asserts the plumbing carries values through unchanged.
    provider = DiffusionProvider(
        config=DiffusionConfig(climate_calibration=False), model_backend=fake
    )
    tile = provider.generate(seed=5, x=2, y=-3, scale=1)

    assert tile.seed == 5 and tile.x == 2 and tile.y == -3 and tile.scale == 1
    assert tile.elevation.dtype == np.int16
    assert int(tile.elevation[0, 0]) == 1235  # 1234.6 rounds to 1235

    assert tile.climate.dtype == np.uint8
    assert tile.climate[0, 0, 0] == 255  # temperature +40 C, top of range
    assert tile.climate[1, 0, 0] == 0  # seasonality 0, bottom of range
    assert tile.climate[2, 0, 0] == 128  # precipitation 6000 of [0, 12000]
    assert tile.climate[3, 0, 0] == 64  # precip_variability 50 of [0, 200]


def test_injected_backend_receives_exact_call_args():
    fake = _FakeBackend()
    provider = DiffusionProvider(model_backend=fake)
    provider.generate(seed=42, x=7, y=-9, scale=1)
    assert fake.calls == [(42, 7, -9, 1)]


def test_injected_backend_wrong_shape_raises_mismatch():
    provider = DiffusionProvider(model_backend=_BadShapeBackend())
    with pytest.raises(ModelOutputMismatch):
        provider.generate(seed=1, x=0, y=0, scale=1)


def test_injected_backend_wrong_dtype_raises_mismatch():
    provider = DiffusionProvider(model_backend=_BadDtypeBackend())
    with pytest.raises(ModelOutputMismatch, match="dtype"):
        provider.generate(seed=1, x=0, y=0, scale=1)


def test_injected_backend_extra_channel_raises_mismatch():
    provider = DiffusionProvider(model_backend=_ExtraChannelBackend())
    with pytest.raises(ModelOutputMismatch, match="extra channel"):
        provider.generate(seed=1, x=0, y=0, scale=1)


def test_dry_run_wins_over_injected_backend():
    """Documented precedence (see DiffusionProvider._call_model's
    docstring): dry_run=True ALWAYS uses the synthetic stand-in, even with
    a model_backend injected -- dry_run exists purely to exercise the
    surrounding plumbing without any model (real or fake) being invoked."""
    fake = _FakeBackend()
    provider = DiffusionProvider(model_backend=fake, dry_run=True)
    provider.generate(seed=1, x=0, y=0, scale=1)
    assert fake.calls == []  # the fake was never called


# ---------------------------------------------------------------------------
# verify_checkpoint_sha256 -- pure, unit-testable with temp files.
# ---------------------------------------------------------------------------


def test_verify_checkpoint_sha256_passes_on_matching_file(tmp_path):
    f = tmp_path / "checkpoint.bin"
    f.write_bytes(b"pretend checkpoint weights")
    import hashlib

    expected = hashlib.sha256(b"pretend checkpoint weights").hexdigest()
    verify_checkpoint_sha256(f, expected)  # must not raise


def test_verify_checkpoint_sha256_raises_on_mismatch(tmp_path):
    f = tmp_path / "checkpoint.bin"
    f.write_bytes(b"pretend checkpoint weights")
    with pytest.raises(ValueError, match="mismatch"):
        verify_checkpoint_sha256(f, "deadbeef" * 8)


def test_verify_checkpoint_sha256_refuses_unpinned(tmp_path):
    f = tmp_path / "checkpoint.bin"
    f.write_bytes(b"pretend checkpoint weights")
    with pytest.raises(ValueError, match="UNPINNED"):
        verify_checkpoint_sha256(f, "UNPINNED")


# ---------------------------------------------------------------------------
# Import without torch: the implicit invariant this module must preserve.
# ---------------------------------------------------------------------------


def test_module_imports_without_torch(monkeypatch):
    """Importing terrain_service.providers.diffusion (and constructing
    DiffusionProvider / TerrainDiffusionBackend) must never require torch --
    only TerrainDiffusionBackend.generate_rasters, lazily, does. Simulate
    torch being entirely absent and re-import fresh to prove it."""
    import builtins
    import importlib
    import sys

    real_import = builtins.__import__

    def _blocking_import(name, *args, **kwargs):
        if name == "torch" or name.startswith("torch."):
            raise ImportError(f"simulated: {name!r} is not installed")
        return real_import(name, *args, **kwargs)

    monkeypatch.delitem(sys.modules, "torch", raising=False)
    monkeypatch.setattr(builtins, "__import__", _blocking_import)

    module_name = "terrain_service.providers.diffusion"
    saved = sys.modules.pop(module_name, None)
    try:
        reloaded = importlib.import_module(module_name)
        # Construction (real, non-dry-run, no injected backend) also must
        # not touch torch -- only generate()/_call_model does.
        provider = reloaded.DiffusionProvider(dry_run=False)
        assert provider.provider_id
    finally:
        monkeypatch.undo()
        sys.modules.pop(module_name, None)
        if saved is not None:
            sys.modules[module_name] = saved
            # Also restore the PACKAGE ATTRIBUTE. importlib.import_module
            # above rebound terrain_service.providers.diffusion to the freshly
            # loaded module object, and `from terrain_service.providers import
            # diffusion` resolves via that attribute, not sys.modules. Leaving
            # it pointing at the throwaway module made every later test that
            # monkeypatches a module-level constant silently patch a module
            # nobody else was using -- a test that could only ever pass.
            setattr(sys.modules["terrain_service.providers"], "diffusion", saved)
        importlib.import_module(module_name)


# ---------------------------------------------------------------------------
# derive_tile_seed -- deterministic, distinct per neighboring tile.
# ---------------------------------------------------------------------------


def test_derive_tile_seed_deterministic():
    a = derive_tile_seed(seed=1, x=0, y=0, scale=1)
    b = derive_tile_seed(seed=1, x=0, y=0, scale=1)
    assert a == b
    assert isinstance(a, int)


def test_derive_tile_seed_distinct_for_neighboring_tiles():
    base = derive_tile_seed(seed=1, x=0, y=0, scale=1)
    neighbors = [
        derive_tile_seed(seed=1, x=1, y=0, scale=1),
        derive_tile_seed(seed=1, x=0, y=1, scale=1),
        derive_tile_seed(seed=1, x=-1, y=0, scale=1),
        derive_tile_seed(seed=1, x=0, y=0, scale=8),
        derive_tile_seed(seed=2, x=0, y=0, scale=1),
    ]
    all_values = [base] + neighbors
    assert len(set(all_values)) == len(all_values), "every distinct (seed,x,y,scale) must hash distinctly"


def test_terrain_diffusion_backend_constructs_without_torch():
    """TerrainDiffusionBackend.__init__ must not import torch -- only
    generate_rasters (via _load_pipeline) does, lazily."""
    backend = TerrainDiffusionBackend(DiffusionConfig())
    assert backend.config is not None


def test_create_app_pins_full_identity_from_env(tmp_path, monkeypatch):
    """The server can pin every identity-bearing field from the environment
    (label, checkpoint hash, conditioning digest, package version) -- and the
    checkpoint LOAD PATH it is given never shows up in the served id."""
    monkeypatch.setenv("TERRAIN_PROVIDER", "diffusion")
    monkeypatch.setenv("TERRAIN_DIFFUSION_DRY_RUN", "1")
    monkeypatch.setenv("TERRAIN_DIFFUSION_CHECKPOINT_ID", "/workspace/ckpt/td30m")
    monkeypatch.setenv("TERRAIN_DIFFUSION_CHECKPOINT_LABEL", "terrain-diffusion-30m")
    monkeypatch.setenv("TERRAIN_DIFFUSION_CHECKPOINT_SHA256", "b" * 64)
    monkeypatch.setenv("TERRAIN_DIFFUSION_CONDITIONING_DIGEST", "e" * 64)
    monkeypatch.setenv("TERRAIN_DIFFUSION_VERSION", "abc1234")
    monkeypatch.delenv("TERRAIN_DIFFUSION_PROVIDER_ID_OVERRIDE", raising=False)

    app = create_app(cache=TileCache(tmp_path))
    served = app.test_client().get("/healthz").get_json()["provider"]

    expected = (
        DiffusionConfig(
            checkpoint_id="/workspace/ckpt/td30m",
            checkpoint_label="terrain-diffusion-30m",
            checkpoint_sha256="b" * 64,
            conditioning_digest="e" * 64,
            terrain_diffusion_version="abc1234",
        ).provider_id()
        + DRYRUN
    )
    assert served == expected
    assert "workspace" not in served
    assert "UNPINNED" not in served and "UNVERIFIED" not in served


def test_create_app_provider_id_override_serves_legacy_namespace(tmp_path, monkeypatch):
    """The migration path for tiles already on disk under the pre-v2 id: the
    server serves that namespace verbatim.

    Non-dry-run, because that is the real scenario (serving tonight's cached
    tiles) and because the ``-dryrun`` suffix is applied on top of the
    override on purpose — dry-run output must never land in a namespace that
    holds real generated tiles, override or not.
    """
    legacy = "terrain-diffusion-/workspace/ckpt/terrain-diffusion-30m-c6f775d3f1b21289"
    monkeypatch.setenv("TERRAIN_PROVIDER", "diffusion")
    monkeypatch.delenv("TERRAIN_DIFFUSION_DRY_RUN", raising=False)
    monkeypatch.setenv("TERRAIN_DIFFUSION_PROVIDER_ID_OVERRIDE", legacy)

    app = create_app(cache=TileCache(tmp_path))
    assert app.test_client().get("/healthz").get_json()["provider"] == legacy

    monkeypatch.setenv("TERRAIN_DIFFUSION_DRY_RUN", "1")
    dry_app = create_app(cache=TileCache(tmp_path))
    assert (
        dry_app.test_client().get("/healthz").get_json()["provider"]
        == legacy + DRYRUN
    )


# ---------------------------------------------------------------------------
# Further inputs that decide tile bytes but live outside DiffusionConfig.
# ---------------------------------------------------------------------------


def test_provider_id_covers_tile_codec_container_format(monkeypatch):
    """A codec bump changes every cached byte string. Cache files are keyed
    only by provider_id, so without this the namespace would end up holding
    two incompatible formats and decode() would raise "unsupported tile
    version" on the older half."""
    before = DiffusionConfig().provider_id()
    monkeypatch.setattr(tile_codec, "VERSION", tile_codec.VERSION + 1)
    assert DiffusionConfig().provider_id() != before


def test_provider_id_covers_generation_algorithm_version(monkeypatch):
    """The manual counter for generation logic that cannot be hashed
    automatically: the axis mapping, the upsampling math, derive_tile_seed's
    scheme. See GENERATION_ALGORITHM_VERSION's docstring for the bump list."""
    from terrain_service.providers import diffusion as diff_mod

    before = DiffusionConfig().provider_id()
    monkeypatch.setattr(diff_mod, "GENERATION_ALGORITHM_VERSION", 99)
    assert DiffusionConfig().provider_id() != before


def test_dry_run_id_carries_the_stand_ins_version():
    """In dry-run mode SyntheticProvider, not the checkpoint, decides the
    bytes -- so bumping "synthetic-v1" must roll the dry-run namespace."""
    dry = DiffusionProvider(dry_run=True).provider_id
    assert dry.endswith(SyntheticProvider.provider_id)
    assert "-dryrun-" in dry


def test_channel_mapping_cannot_be_mutated_after_construction():
    """frozen=True blocks rebinding but not mutation, and the mapping is read
    live on every generate() while provider_id was snapshotted at
    construction -- one mutation would repack the climate planes under an
    already-published identity."""
    config = DiffusionConfig()
    with pytest.raises(TypeError):
        config.channel_mapping["temperature"] = "t2m"

    # Constructing FROM a dict must copy, so later mutating the caller's dict
    # cannot reach in either.
    caller_dict = dict(DEFAULT_CHANNEL_MAPPING)
    config2 = DiffusionConfig(channel_mapping=caller_dict)
    before = config2.provider_id()
    caller_dict["temperature"] = "t2m"
    assert config2.provider_id() == before


def test_world_shape_defaults_are_pinned():
    """WorldShapeConfig's defaults must reproduce upstream WorldPipeline's.

    They are a COPY of `world_pipeline.py`'s `__init__` defaults, taken
    2026-07-25. A copy can go stale silently: upstream changes a default, we
    keep passing the old value, and tiles change character with no signal. This
    cannot detect that on its own -- terrain-diffusion is not importable in this
    (torch-free) test environment -- but it does pin OUR side, so the copy can
    only drift deliberately, and it documents the values to diff against
    upstream when bumping `terrain_diffusion_version`.
    """
    kw = WorldShapeConfig().as_pipeline_kwargs()
    assert kw == {
        "frequency_mult": [1.5, 3.0, 3.0, 3.0, 3.0],
        "drop_water_pct": 0.5,
        "cond_snr": [0.3, 0.1, 1.0, 0.1, 1.0],
        "coarse_pooling": 1,
        "elev_coarse_pool_mode": "avg",
        "p5_coarse_pool_mode": "avg",
        # NOT an upstream default. `orographic` is OURS: upstream's parameter
        # defaults to None (no coupling), and we deliberately turn it on. It is
        # pinned here for the same reason as the rest -- so the world can only
        # change on purpose -- but do NOT "fix" it by diffing against upstream
        # when bumping terrain_diffusion_version. Setting orographic_enabled
        # False restores upstream's exact behaviour.
        "orographic": {
            "wind_from_deg": 270.0,
            "probe_wavelengths": [0.15, 0.30, 0.60, 1.20],
            "barrier_m": 1200.0,
            "upslope_m": 600.0,
            "shadow_strength": 0.75,
            "enhance_strength": 0.60,
            "sea_blend_m": 200.0,
        },
        # Also OURS, not upstream: upstream defaults elev_gain to 1.0 (the
        # untouched Earth marginal). 1.6 is measured -- see the table in
        # WorldShapeConfig and docs/measurements/elevation-tails-2026-08-01.txt.
        "elev_gain": 1.6,
        "elev_gain_power": 2.0,
    }
    # Lists, not tuples: upstream indexes and slices these.
    assert isinstance(kw["frequency_mult"], list)
    assert isinstance(kw["cond_snr"], list)


def test_world_shape_is_hashable_and_frozen():
    """The config is snapshotted into provider_id at construction, so a mutable
    field would let the world change under an already-published identity --
    the same class of bug __post_init__ freezes channel_mapping against."""
    import dataclasses

    ws = WorldShapeConfig()
    hash(ws)  # tuples, not lists, on the dataclass itself
    with pytest.raises(dataclasses.FrozenInstanceError):
        ws.drop_water_pct = 0.1


def test_clamp_to_physical_range_pulls_in_small_negative_precipitation():
    """The real failure: an all-arid tile with precipitation slightly below 0.

    Tile (-4, 19) on the pod came back with precipitation in
    [-98.83, 361.55] -- a whole desert tile. Negative rainfall is noise about
    a floor the model does not know exists, not a prediction, and refusing it
    would block exactly the arid terrain the conditioning work produces.
    """
    raster = _good_raster()
    raster["precipitation"] = np.full((TILE_SIZE, TILE_SIZE), 200.0, dtype=np.float32)
    raster["precipitation"][0, 0] = -98.83
    n = clamp_to_physical_range(raster, DEFAULT_CHANNEL_MAPPING)
    assert n == {"precipitation": 1}
    assert raster["precipitation"][0, 0] == 0.0
    # And it now passes validation, which it did not before.
    validate_model_output(raster, DEFAULT_CHANNEL_MAPPING)


def test_clamp_to_physical_range_does_not_hide_real_breakage():
    """A blanket clip would mask the saturation bug adapt_raster_to_tile warns of.

    Excursions beyond the tolerance must survive into validate_model_output
    and still fail there.
    """
    raster = _good_raster()
    # 5% of precipitation's [0, 12000] span is 600; -5000 is far outside.
    raster["precipitation"] = np.full((TILE_SIZE, TILE_SIZE), -5000.0, dtype=np.float32)
    assert clamp_to_physical_range(raster, DEFAULT_CHANNEL_MAPPING) == {}
    assert raster["precipitation"][0, 0] == -5000.0
    with pytest.raises(ModelOutputMismatch):
        validate_model_output(raster, DEFAULT_CHANNEL_MAPPING)


def test_clamp_to_physical_range_leaves_in_range_values_untouched():
    raster = _good_raster()
    before = {k: v.copy() for k, v in raster.items()}
    assert clamp_to_physical_range(raster, DEFAULT_CHANNEL_MAPPING) == {}
    for k, v in raster.items():
        assert np.array_equal(v, before[k]), k
