"""terrain-diffusion provider — the CANONICAL tile source (plan §3.1 step 2).

Wraps https://github.com/xandergos/terrain-diffusion as an async GPU worker.
Diffusion output is NOT bit-deterministic across GPUs (doctrine §2.3), which
is exactly why tiles are generated once, cached forever, and distributed as
data.

Bring-up status: everything that does NOT need a GPU is implemented and
tested here — config, adapter, validation, dry-run plumbing. The one thing
that needs a CUDA machine is clearly marked with a TODO in
``DiffusionProvider._call_model``. See ``terrain-service/docs/diffusion-bringup.md``
for the runbook.

Design, top to bottom:

  * ``DiffusionConfig`` — the pinned bring-up config (checkpoint id/hash,
    sampler settings, tile scale, channel mapping). It is entirely data, and
    it feeds ``provider_id()`` per doctrine §2.3 ("Determinism boundary"):
    regenerating the cache is driven by ANY change to this config, not just
    a checkpoint swap.
  * ``EXPECTED_CHANNELS`` — our current assumption about what the model
    emits: int16-metres elevation + 4 uint8 climate channels (temperature,
    seasonality, precipitation, precip variability), per
    ``docs/voxel-earth-implementation-plan.md`` §3.1 and ``docs/m4-plan.md``.
    This is exactly the assumption that backlog item "Confirm real
    terrain-diffusion tile outputs" (docs/status.md) needs confirmed.
  * ``validate_model_output`` — checks a raw raster dict (as the model would
    hand it back: float arrays keyed by channel name) against
    ``EXPECTED_CHANNELS``, resolved through the config's ``channel_mapping``.
    This IS the "confirm real outputs" tool: point it at one real inference
    result at bring-up time and it reports exactly how reality differs from
    our assumption (missing/extra channels, wrong dtype, out-of-range
    values) instead of silently miscoding climate data.
  * ``adapt_raster_to_tile`` — converts a validated raster dict into our
    ``Tile`` wire format (int16 elevation metres, uint8x4 climate). If the
    real model's channel *names* differ from ours, that's a
    ``DiffusionConfig.channel_mapping`` edit, not a code change (task
    scaffolding item 1).
  * ``DiffusionProvider`` — the ``TileProvider``. In ``dry_run=True`` mode
    (default off) it swaps the real model call for the synthetic
    provider's rasters reshaped to look like model output, then runs the
    SAME config -> adapt -> validate -> encode path a real bring-up would.
    This proves the plumbing end-to-end without a GPU.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field

import numpy as np

from ..tile_codec import CLIMATE_CHANNELS, PIXEL_SIZE_MM, Tile

# ---------------------------------------------------------------------------
# Expected model output — our current assumption (plan §3.1, m4-plan.md).
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class ChannelSpec:
    """One raster channel we expect the diffusion model to emit."""

    name: str
    dtype: str  # numpy dtype string, e.g. "float32"
    min: float
    max: float


#: Our pipeline's current assumption about the model's raw (pre-adapter)
#: output: elevation in metres as float32, plus 4 climate channels
#: normalized to [0, 1] float32 (the adapter quantizes these into the
#: int16/uint8 wire format). This is the manifest the "confirm real
#: outputs" backlog item (docs/status.md) verifies against a real
#: checkpoint. Ranges are generous (Earth's extremes) so validation only
#: fires on genuinely wrong output, not legitimate high mountains/trenches.
EXPECTED_CHANNELS: tuple[ChannelSpec, ...] = (
    ChannelSpec("elevation", "float32", -12000.0, 9000.0),
    ChannelSpec("temperature", "float32", 0.0, 1.0),
    ChannelSpec("seasonality", "float32", 0.0, 1.0),
    ChannelSpec("precipitation", "float32", 0.0, 1.0),
    ChannelSpec("precip_variability", "float32", 0.0, 1.0),
)

#: Identity mapping: semantic channel name -> raster dict key the model
#: uses. Override via ``DiffusionConfig(channel_mapping=...)`` if the real
#: checkpoint names (or splits/merges) channels differently — a config
#: change, not a code change.
DEFAULT_CHANNEL_MAPPING: dict[str, str] = {c.name: c.name for c in EXPECTED_CHANNELS}

#: Order climate channels are packed into the tile's uint8x4 plane, per
#: tile_codec.py's docstring (temperature, seasonality, precipitation,
#: precip variability).
_CLIMATE_ORDER: tuple[str, ...] = (
    "temperature",
    "seasonality",
    "precipitation",
    "precip_variability",
)


class ModelOutputMismatch(ValueError):
    """Raised by validate_model_output; .issues lists every mismatch found."""

    def __init__(self, issues: list[str]) -> None:
        self.issues = issues
        super().__init__(
            "diffusion model output does not match EXPECTED_CHANNELS "
            f"({len(issues)} issue(s)):\n" + "\n".join(f"  - {i}" for i in issues)
        )


def validate_model_output(
    raster_dict: dict[str, np.ndarray],
    channel_mapping: dict[str, str] | None = None,
) -> None:
    """Check a raw model output dict against EXPECTED_CHANNELS.

    ``raster_dict`` is what a bring-up session gets back directly from
    terrain-diffusion inference: channel name -> 2D array. ``channel_mapping``
    resolves OUR semantic names (elevation, temperature, ...) to the model's
    raster keys (identity by default; override when checkpoint naming
    differs from ours).

    Checks, per expected channel: present under its mapped key, correct
    dtype, correct shape (square, consistent across channels), and value
    range. Also flags any raster keys left over after mapping (extra
    channels the model emits that we don't expect/consume) and any expected
    channel whose mapped key is entirely absent (channel COUNT mismatch).

    Raises ``ModelOutputMismatch`` (a ``ValueError``) listing every issue
    found — not just the first — so a bring-up session sees the whole
    picture in one run. Raises nothing (returns None) if everything matches.
    """
    mapping = channel_mapping or DEFAULT_CHANNEL_MAPPING
    issues: list[str] = []

    expected_keys = set()
    for spec in EXPECTED_CHANNELS:
        raster_key = mapping.get(spec.name)
        if raster_key is None:
            issues.append(
                f"channel_mapping has no entry for expected channel {spec.name!r}"
            )
            continue
        expected_keys.add(raster_key)
        if raster_key not in raster_dict:
            issues.append(
                f"missing channel: expected {spec.name!r} at raster key "
                f"{raster_key!r} (not present in model output)"
            )
            continue

        arr = raster_dict[raster_key]
        if not isinstance(arr, np.ndarray):
            issues.append(
                f"channel {spec.name!r} ({raster_key!r}): expected numpy array, "
                f"got {type(arr).__name__}"
            )
            continue
        if arr.dtype != np.dtype(spec.dtype):
            issues.append(
                f"channel {spec.name!r} ({raster_key!r}): expected dtype "
                f"{spec.dtype}, got {arr.dtype}"
            )
        if arr.ndim != 2 or arr.shape[0] != arr.shape[1]:
            issues.append(
                f"channel {spec.name!r} ({raster_key!r}): expected a square 2D "
                f"raster, got shape {arr.shape}"
            )
        elif arr.size:
            lo, hi = float(arr.min()), float(arr.max())
            if lo < spec.min or hi > spec.max:
                issues.append(
                    f"channel {spec.name!r} ({raster_key!r}): value range "
                    f"[{lo:g}, {hi:g}] outside expected [{spec.min:g}, {spec.max:g}]"
                )

    extra = set(raster_dict) - expected_keys
    if extra:
        issues.append(
            "unexpected extra channel(s) in model output not covered by "
            f"channel_mapping: {sorted(extra)} — either the model emits more "
            "than we consume, or channel_mapping needs updating"
        )

    # Channel COUNT check, stated explicitly (in addition to the per-key
    # presence/extra checks above) so a bring-up session sees a one-line
    # summary of "N expected vs M actual" even when names also disagree.
    if len(raster_dict) != len(EXPECTED_CHANNELS):
        issues.append(
            f"channel count mismatch: expected {len(EXPECTED_CHANNELS)} "
            f"channels, model output has {len(raster_dict)}"
        )

    if issues:
        raise ModelOutputMismatch(issues)


# ---------------------------------------------------------------------------
# Adapter: validated raster dict -> our Tile wire format.
# ---------------------------------------------------------------------------


def adapt_raster_to_tile(
    raster_dict: dict[str, np.ndarray],
    config: "DiffusionConfig",
    seed: int,
    x: int,
    y: int,
    scale: int,
) -> Tile:
    """Convert a (validated) model raster dict into our Tile format.

    Elevation: float32 metres -> int16 metres (rounded, clipped to int16).
    Climate: float32 in [0, 1] -> uint8 in [0, 255] (rounded, clipped),
    packed in the tile_codec channel order (temperature, seasonality,
    precipitation, precip_variability).

    Callers should run ``validate_model_output(raster_dict, config.channel_mapping)``
    first — this function does not re-validate; it trusts the caller and
    focuses purely on the numeric conversion.
    """
    mapping = config.channel_mapping
    elev_key = mapping["elevation"]
    elevation = np.rint(raster_dict[elev_key]).astype(np.int64)
    elevation = np.clip(elevation, -32768, 32767).astype(np.int16)

    climate = np.zeros((CLIMATE_CHANNELS, *elevation.shape), dtype=np.uint8)
    for i, name in enumerate(_CLIMATE_ORDER):
        raw = raster_dict[mapping[name]]
        climate[i] = np.clip(np.rint(raw * 255.0), 0, 255).astype(np.uint8)

    return Tile(seed=seed, x=x, y=y, scale=scale, elevation=elevation, climate=climate)


# ---------------------------------------------------------------------------
# Pinned bring-up config.
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class SamplerConfig:
    """Diffusion sampler settings — part of the pinned config (doctrine §2.3):
    changing any of these changes generated tiles, so it must roll the
    provider_id / cache key."""

    steps: int = 30
    guidance_scale: float = 3.0
    scheduler: str = "ddim"


@dataclass(frozen=True)
class DiffusionConfig:
    """The pinned terrain-diffusion bring-up config.

    Everything a GPU bring-up session needs to pin down before generating
    canonical tiles: which checkpoint, which sampler settings, which tile
    scale it targets, and how the model's raw channel names map onto ours.
    Per doctrine §2.3 ("Determinism boundary"), diffusion output is not
    bit-deterministic across GPUs/versions, so tiles are generated once and
    distributed as data — this config is exactly the set of knobs that must
    be pinned (and folded into ``provider_id()``) so a cache can never
    silently mix tiles from two different configs.
    """

    #: Model checkpoint identifier (e.g. HF repo id or release tag).
    #: "UNPINNED" until a real checkpoint is selected at bring-up.
    checkpoint_id: str = "terrain-diffusion-30m-UNPINNED"
    #: sha256 of the checkpoint file/weights, pinned once known. Prevents a
    #: silent checkpoint swap (same id, different weights) from mixing into
    #: an existing cache under the same provider_id.
    checkpoint_sha256: str = "UNPINNED"
    sampler: SamplerConfig = field(default_factory=SamplerConfig)
    #: Tile pixel scale this config is calibrated for (tile_codec.PIXEL_SIZE_MM
    #: key: 1 => 30m/px, 8 => 11.25m/px supersampled). Must match the `scale`
    #: argument the provider is actually called with.
    scale: int = 1
    #: Semantic channel name -> raster dict key the model emits. Identity
    #: by default; edit at bring-up if the real checkpoint's raster keys
    #: differ from ours (config change, not code change — see module
    #: docstring and adapt_raster_to_tile).
    channel_mapping: dict[str, str] = field(
        default_factory=lambda: dict(DEFAULT_CHANNEL_MAPPING)
    )

    def __post_init__(self) -> None:
        if self.scale not in PIXEL_SIZE_MM:
            raise ValueError(f"unsupported scale {self.scale}, must be one of {sorted(PIXEL_SIZE_MM)}")
        missing = {c.name for c in EXPECTED_CHANNELS} - set(self.channel_mapping)
        if missing:
            raise ValueError(f"channel_mapping missing entries for: {sorted(missing)}")

    def provider_id(self) -> str:
        """Stable identity+version string for the cache key (TileProvider
        contract). Deterministic hash of every field that can change
        generated bytes — if a bring-up session edits ANY of checkpoint,
        sampler, scale, or channel_mapping, this changes, so old and new
        tiles never collide in the cache."""
        payload = {
            "checkpoint_id": self.checkpoint_id,
            "checkpoint_sha256": self.checkpoint_sha256,
            "sampler": {
                "steps": self.sampler.steps,
                "guidance_scale": self.sampler.guidance_scale,
                "scheduler": self.sampler.scheduler,
            },
            "scale": self.scale,
            "channel_mapping": self.channel_mapping,
        }
        digest = hashlib.sha256(
            json.dumps(payload, sort_keys=True).encode("utf-8")
        ).hexdigest()[:16]
        return f"terrain-diffusion-{self.checkpoint_id}-{digest}"


# ---------------------------------------------------------------------------
# Provider.
# ---------------------------------------------------------------------------


class DiffusionProvider:
    """The canonical tile provider, wrapping terrain-diffusion.

    Construction never touches a GPU. Set ``dry_run=True`` to exercise the
    full config -> (stand-in model call) -> adapt -> validate -> encode
    pipeline using the synthetic provider's rasters reshaped to look like
    model output — this is how the plumbing is proven correct before any
    GPU exists. With ``dry_run=False`` (the default, and what production
    uses), ``generate()`` raises ``NotImplementedError`` at the one place
    that needs a real CUDA machine (``_call_model``); see
    ``terrain-service/docs/diffusion-bringup.md``.
    """

    def __init__(
        self, config: DiffusionConfig | None = None, dry_run: bool = False
    ) -> None:
        self.config = config or DiffusionConfig()
        self.dry_run = dry_run
        # provider_id is an attribute (not a property) per the TileProvider
        # protocol; dry-run runs are tagged so they can never collide with
        # real generated tiles in a shared cache.
        base_id = self.config.provider_id()
        self.provider_id = f"{base_id}-dryrun" if dry_run else base_id

    def generate(self, seed: int, x: int, y: int, scale: int) -> Tile:
        if scale != self.config.scale:
            raise ValueError(
                f"DiffusionProvider configured for scale={self.config.scale}, "
                f"got scale={scale} — bring up a separate DiffusionConfig "
                "(and provider_id) per scale"
            )
        raster = self._call_model(seed, x, y, scale)
        validate_model_output(raster, self.config.channel_mapping)
        return adapt_raster_to_tile(raster, self.config, seed, x, y, scale)

    def _call_model(self, seed: int, x: int, y: int, scale: int) -> dict[str, np.ndarray]:
        """The one place that needs a real CUDA machine.

        TODO(GPU bring-up, see docs/diffusion-bringup.md):
          1. Load ``self.config.checkpoint_id`` (verify against
             ``self.config.checkpoint_sha256``) via terrain-diffusion.
          2. Run inference with ``self.config.sampler`` settings, seeded/
             positioned from (seed, x, y, scale) per terrain-diffusion's
             sampling API (tile (x, y) at TILE_SIZE covers pixels
             [x*TILE_SIZE, (x+1)*TILE_SIZE) etc. — same convention as
             tile_codec.py).
          3. Return a dict of channel name -> float32 numpy array, keyed
             per ``self.config.channel_mapping`` values, at
             (TILE_SIZE, TILE_SIZE) resolution. Do NOT quantize here —
             ``validate_model_output``/``adapt_raster_to_tile`` handle that;
             this function's job is exactly "get the model's raw raster
             out", so ``validate_model_output`` can compare it honestly
             against EXPECTED_CHANNELS.

        Until that lands, dry_run=False callers get a clear error instead of
        silently returning fake data; dry_run=True callers get the
        synthetic-provider stand-in below (proves the plumbing, not the
        model).
        """
        if self.dry_run:
            return self._synthetic_stand_in(seed, x, y, scale)
        raise NotImplementedError(
            "terrain-diffusion model call not wired up yet — needs a CUDA "
            "machine. See terrain-service/docs/diffusion-bringup.md for the "
            "bring-up runbook, or pass dry_run=True to exercise the "
            "surrounding plumbing (config -> adapter -> validate -> encode) "
            "with synthetic stand-in rasters."
        )

    def _synthetic_stand_in(
        self, seed: int, x: int, y: int, scale: int
    ) -> dict[str, np.ndarray]:
        """Synthetic-fallback rasters shaped exactly like real model output
        (float32, channel-named per config.channel_mapping) so dry-run mode
        exercises the SAME validate/adapt path a real bring-up will, without
        a GPU. Uses SyntheticProvider purely as a source of plausible
        numbers — NOT a substitute for real terrain-diffusion output."""
        # Local import: keeps the (GPU-bound, eventually heavy) diffusion
        # module from depending on the dev-only synthetic provider at
        # import time; only dry_run pulls it in.
        from .synthetic import SyntheticProvider

        tile = SyntheticProvider().generate(seed, x, y, scale)
        mapping = self.config.channel_mapping
        raster: dict[str, np.ndarray] = {
            mapping["elevation"]: tile.elevation.astype(np.float32),
        }
        for i, name in enumerate(_CLIMATE_ORDER):
            raster[mapping[name]] = (tile.climate[i].astype(np.float32)) / 255.0
        return raster
