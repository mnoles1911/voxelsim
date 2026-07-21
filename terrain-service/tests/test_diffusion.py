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
    DEFAULT_CHANNEL_MAPPING,
    EXPECTED_CHANNELS,
    DiffusionConfig,
    DiffusionProvider,
    ModelOutputMismatch,
    SamplerConfig,
    TerrainDiffusionBackend,
    adapt_raster_to_tile,
    derive_tile_seed,
    validate_model_output,
    verify_checkpoint_sha256,
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
            "elevation": np.full((TILE_SIZE, TILE_SIZE), 1234.6, dtype=np.float32),
            "temperature": np.full((TILE_SIZE, TILE_SIZE), 1.0, dtype=np.float32),
            "seasonality": np.full((TILE_SIZE, TILE_SIZE), 0.0, dtype=np.float32),
            "precipitation": np.full((TILE_SIZE, TILE_SIZE), 0.5, dtype=np.float32),
            "precip_variability": np.full((TILE_SIZE, TILE_SIZE), 0.25, dtype=np.float32),
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
    provider = DiffusionProvider(model_backend=fake)
    tile = provider.generate(seed=5, x=2, y=-3, scale=1)

    assert tile.seed == 5 and tile.x == 2 and tile.y == -3 and tile.scale == 1
    assert tile.elevation.dtype == np.int16
    assert int(tile.elevation[0, 0]) == 1235  # 1234.6 rounds to 1235

    assert tile.climate.dtype == np.uint8
    assert tile.climate[0, 0, 0] == 255  # temperature 1.0 -> 255
    assert tile.climate[1, 0, 0] == 0  # seasonality 0.0 -> 0
    assert tile.climate[2, 0, 0] == 128  # precipitation 0.5 -> 128 (rounds)
    assert tile.climate[3, 0, 0] == 64  # precip_variability 0.25 -> 64 (rounds)


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
