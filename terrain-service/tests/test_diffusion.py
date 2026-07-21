"""Diffusion provider bring-up scaffolding tests (no GPU): validate_model_output,
the dry-run config->adapter->validate->encode pipeline, and provider_id
stability. Everything here MUST pass without a CUDA machine — the real model
call stays behind DiffusionProvider._call_model's TODO."""

from __future__ import annotations

import numpy as np
import pytest

from terrain_service import tile_codec
from terrain_service.app import _make_provider, create_app
from terrain_service.cache import TileCache
from terrain_service.providers.diffusion import (
    DEFAULT_CHANNEL_MAPPING,
    EXPECTED_CHANNELS,
    DiffusionConfig,
    DiffusionProvider,
    ModelOutputMismatch,
    SamplerConfig,
    adapt_raster_to_tile,
    validate_model_output,
)

TILE_SIZE = tile_codec.TILE_SIZE


def _good_raster(mapping: dict[str, str] | None = None) -> dict[str, np.ndarray]:
    mapping = mapping or DEFAULT_CHANNEL_MAPPING
    raster = {mapping["elevation"]: np.full((TILE_SIZE, TILE_SIZE), 100.0, dtype=np.float32)}
    for name in ("temperature", "seasonality", "precipitation", "precip_variability"):
        raster[mapping[name]] = np.full((TILE_SIZE, TILE_SIZE), 0.5, dtype=np.float32)
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
    raster["precipitation"] = np.full((TILE_SIZE, TILE_SIZE), 5.0, dtype=np.float32)
    with pytest.raises(ModelOutputMismatch) as exc:
        validate_model_output(raster)
    msg = str(exc.value)
    assert "precipitation" in msg and "range" in msg


def test_validate_model_output_reports_all_issues_at_once():
    """Bring-up sessions want the whole mismatch picture in one run, not
    one issue per re-run."""
    raster = _good_raster()
    raster["elevation"] = raster["elevation"].astype(np.float64)  # dtype issue
    raster["precipitation"] = np.full((TILE_SIZE, TILE_SIZE), 5.0, dtype=np.float32)  # range issue
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
    config = DiffusionConfig()
    raster = _good_raster()
    raster["elevation"] = np.full((TILE_SIZE, TILE_SIZE), 1234.6, dtype=np.float32)
    raster["temperature"] = np.full((TILE_SIZE, TILE_SIZE), 1.0, dtype=np.float32)
    tile = adapt_raster_to_tile(raster, config, seed=1, x=0, y=0, scale=1)
    assert tile.elevation.dtype == np.int16
    assert int(tile.elevation[0, 0]) == 1235  # rounds
    assert tile.climate.dtype == np.uint8
    assert tile.climate[0, 0, 0] == 255  # temperature channel 0, scaled 1.0 -> 255


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
    base = DiffusionConfig().provider_id()
    diff_checkpoint = DiffusionConfig(checkpoint_id="other-checkpoint").provider_id()
    diff_hash = DiffusionConfig(checkpoint_sha256="deadbeef").provider_id()
    diff_sampler = DiffusionConfig(sampler=SamplerConfig(steps=50)).provider_id()
    diff_scale = DiffusionConfig(scale=8).provider_id()
    diff_mapping = DiffusionConfig(
        channel_mapping={**DEFAULT_CHANNEL_MAPPING, "precip_variability": "precip_var"}
    ).provider_id()
    ids = {base, diff_checkpoint, diff_hash, diff_sampler, diff_scale, diff_mapping}
    assert len(ids) == 6, "every config field must roll the provider_id"


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
    assert dry.provider_id.endswith("-dryrun")

    # Real (non-dry-run) construction never touches a GPU — only generate()
    # does — so this must succeed without raising.
    real = DiffusionProvider(dry_run=False)
    assert not real.provider_id.endswith("-dryrun")
    assert real.provider_id != dry.provider_id


def test_dry_run_provider_deterministic():
    provider = DiffusionProvider(dry_run=True)
    a = provider.generate(7, 3, 3, 1)
    b = provider.generate(7, 3, 3, 1)
    np.testing.assert_array_equal(a.elevation, b.elevation)
    np.testing.assert_array_equal(a.climate, b.climate)


def test_non_dry_run_generate_raises_clear_not_implemented():
    provider = DiffusionProvider(dry_run=False)
    with pytest.raises(NotImplementedError, match="CUDA machine"):
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
