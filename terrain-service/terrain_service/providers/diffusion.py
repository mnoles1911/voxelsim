"""terrain-diffusion provider — the CANONICAL tile source (plan §3.1 step 2).

Wraps https://github.com/xandergos/terrain-diffusion as an async GPU worker.
Diffusion output is NOT bit-deterministic across GPUs (doctrine §2.3), which
is exactly why tiles are generated once, cached forever, and distributed as
data.

Bring-up status: everything that does NOT need a GPU is implemented and
tested here — config, adapter, validation, dry-run plumbing, AND the real
model call itself (``TerrainDiffusionBackend``), which is fully wired up but
untestable without a CUDA machine (module import and construction never
touch torch; only ``TerrainDiffusionBackend.generate_rasters`` does, lazily).
See ``terrain-service/docs/diffusion-bringup.md`` for the remaining
GPU-session runbook (pin a checkpoint, run validation, sanity-check the
ASSUMPTIONs called out on ``TerrainDiffusionBackend``).

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
  * ``provider_id`` — see ``DiffusionConfig.provider_id``. It is the tile-
    cache namespace AND the value stamped into edit logs that
    ``EditLog::checkProvider()`` compares before replaying a saved world
    (``kMismatch`` = refuse the replay), so it must be content-addressed:
    every input that can change tile bytes, and nothing about where those
    bytes live. Identity schema v2 (2026-07-22) fixed two bugs found on the
    first real generation run — the local checkpoint LOAD PATH was embedded
    in it, and the conditioning data (WorldClim bio rasters +
    ``data/global/etopo_10m.tif``, which condition generation via
    ``synthetic_map._compute_map_stats``) was not hashed at all.

    KNOWN REMAINING GAPS, in rough priority order — inputs that can still
    change tile bytes without rolling the id:

      1. FIXED 2026-07-25 (identity schema v3). ``SamplerConfig`` was hashed
         but never passed to anything; it is replaced by ``WorldShapeConfig``,
         which carries the ``WorldPipeline`` kwargs that really do shape the
         world (``frequency_mult``, ``drop_water_pct``, ``cond_snr``,
         ``coarse_pooling``, the pool modes) and IS forwarded to
         ``from_pretrained``. See ``docs/worldgen-levers.md``.
      2. The execution environment. ``_load_pipeline`` silently falls back
         to ``device="cpu"`` when no GPU is visible, so CPU- and
         GPU-generated tiles share a namespace; ``torch``/cuDNN versions and
         TF32 flags are likewise absent. Doctrine §2.3 accepts cross-GPU
         non-determinism, but the id does not even record which side of the
         CPU/GPU split a tile came from.
      3. ``terrain_diffusion_version`` defaults to ``"UNRECORDED"`` and,
         unlike ``UNPINNED``/``UNVERIFIED``, is neither refused before
         inference nor marked in the id.
      4. ``pipeline.bind()``'s caching strategy. ``(x, y)`` are not
         independent per-tile seeds; seamlessness comes from the tile
         store's cached context, so which neighbours are resident is
         process-history state that can influence output.

  * ``DiffusionProvider`` — the ``TileProvider``. In ``dry_run=True`` mode
    (default off) it swaps the real model call for the synthetic
    provider's rasters reshaped to look like model output, then runs the
    SAME config -> adapt -> validate -> encode path a real bring-up would.
    This proves the plumbing end-to-end without a GPU.
"""

from __future__ import annotations

import hashlib
import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from types import MappingProxyType
from typing import Protocol

import numpy as np

from ..tile_codec import CLIMATE_CHANNELS, PIXEL_SIZE_MM, TILE_SIZE, Tile

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


#: What the model actually emits (CONFIRMED against a real checkpoint on an
#: RTX 4090, 2026-07-22 — the "confirm real terrain-diffusion tile outputs"
#: backlog item). Elevation is float32 metres. The four climate channels are
#: **raw WorldClim bioclim values in physical units**, NOT normalized to
#: [0, 1] as originally assumed:
#:
#:   temperature        bio_1   annual mean temperature, degrees C
#:   seasonality        bio_4   temperature seasonality, sd(monthly) x 100
#:   precipitation      bio_12  annual precipitation, mm
#:   precip_variability bio_15  precipitation seasonality, CV %
#:
#: That identification is not a guess: the pipeline downloads the WorldClim
#: 2.1 10-arc-minute bio rasters at bring-up and conditions on them, and the
#: observed tile-(0,0) values (21.1-22.8, 451-548, 629-772, 42-58) sit exactly
#: where bio_1/4/12/15 sit for a warm wet ocean cell.
#:
#: These bounds do double duty: they are the validation range AND the
#: quantization range the adapter maps into uint8 (see adapt_raster_to_tile).
#: So they are part of the tile wire format — changing one changes tile bytes,
#: and must roll the provider_id. They are set generously (Earth's extremes,
#: e.g. bio_12 max ~11,900 mm at Mawsynram) so validation fires only on
#: genuinely wrong output, and so quantization clips only at true extremes.
EXPECTED_CHANNELS: tuple[ChannelSpec, ...] = (
    ChannelSpec("elevation", "float32", -12000.0, 9000.0),
    ChannelSpec("temperature", "float32", -40.0, 40.0),
    ChannelSpec("seasonality", "float32", 0.0, 3000.0),
    ChannelSpec("precipitation", "float32", 0.0, 12000.0),
    ChannelSpec("precip_variability", "float32", 0.0, 200.0),
)

#: Climate channels by name, for the adapter's per-channel normalization.
_CLIMATE_SPECS: dict[str, ChannelSpec] = {
    c.name: c for c in EXPECTED_CHANNELS if c.name != "elevation"
}

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

#: Fine (30 m) pixels per cell of the model's COARSE stage, on both axes.
#:
#: ``world_pipeline.py::_compute_climate`` indexes ``self.coarse`` at
#: ``i // (32 * scale)`` with ``scale = latent_compression``; the shipped
#: checkpoint has ``latent_compression = 8``, so 256. That is where the
#: "one coarse cell is 7.68 km" everything in this repo quotes comes from, and
#: it makes a coarse cell exactly half a tile (``TILE_SIZE`` = 512).
#:
#: ``TerrainDiffusionBackend.coarse_cell_fine_px`` re-derives this from the
#: LIVE pipeline and refuses if it disagrees, because a wrong ratio silently
#: georeferences a whole coarse window to the wrong ground.
FINE_PX_PER_COARSE_CELL = 256


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


#: MODEL-OUTPUT CLIMATE CALIBRATION -- fitted, not guessed.
#:
#: THE PROBLEM, measured across 10 windows spanning ~2,600 km at seed
#: 20260719: the coarse model obeys the elevation conditioning (asks 22.2% of
#: land above 1 km, delivers 24.3%) but CRUSHES the climate channels. Land
#: temperature came back capped at ~20.5 C p95 / ~25 C max in EVERY window
#: while the conditioning asked for 28.5 / 33.9. The consequence was total:
#: cells that are simultaneously hot (>=24 C) and arid (<400 mm) were 10.04%
#: of land as ASKED and 0.00% as DELIVERED, everywhere. No choice of pregen
#: origin fixes that, and no biome threshold can honestly invent a desert out
#: of climate that contains none.
#:
#: WHY A MONOTONE REMAP IS THE RIGHT FIX. The model preserves each channel's
#: spatial PATTERN and squashes its RANGE. A strictly monotone per-pixel map
#: therefore restores the range while leaving spatial structure *exactly*
#: untouched -- it cannot move a warm cell to a different place, only relabel
#: how warm it is. It is the same quantile-matching idea the conditioning
#: already uses on the input side, applied on the output side.
#:
#: WHY IT CANNOT SEAM. These are FIXED GLOBAL CONSTANTS, a pure function of
#: the value alone. A per-tile empirical quantile match would seam badly: each
#: tile has its own local distribution, so the same physical temperature would
#: encode differently on either side of a tile border.
#:
#: Fitted by pairing pooled DELIVERED land quantiles with pooled ASKED land
#: quantiles (the conditioning target, itself built from real WorldClim).
#: Regenerate with tools/fit_climate_calibration.py if the model, the
#: conditioning stats, or elev_gain change -- all three move these curves.
#: See docs/measurements/climate-calibration-2026-08-01.txt.
#: FITTED ON THE FULL PIPELINE, on a GPU. The first version of these curves
#: was fitted on the COARSE STAGE ALONE, because the dev box is CPU-only torch
#: on an AMD card and the latent/decoder stages cannot run there. That was
#: wrong, and only a real pod could show it:
#:
#:   raw precipitation p50 -- coarse stage 680 mm, FULL PIPELINE 1788 mm
#:
#: 2.6x wetter. The coarse-fitted curve was being fed values far above its top
#: anchor, so it extrapolated and made an already-wet world wetter still
#: (delivered p50 2118 mm against a 554 mm target). Temperature was off the
#: same way in the other direction: raw p95 is 25.9 on the full pipeline
#: against 20.5 on coarse, so the coarse curve over-stretched it to 32.8.
#:
#: Re-fitted over 25 tiles / 2,800,790 land pixels against the co-located
#: conditioning sketch. Regenerate with tools/fit_climate_calibration.py --
#: and regenerate it ON A MACHINE THAT CAN RUN THE DECODER, or this mistake
#: repeats.
CLIMATE_CALIBRATION: dict[str, tuple[tuple[float, ...], tuple[float, ...]]] = {
    "temperature": (
        (-3.922, 1.412, 9.882, 14.902, 19.294, 25.882, 26.510),
        (-10.984, -3.108, 10.921, 21.435, 24.753, 28.086, 30.512),
    ),
    "seasonality": (
        (470.588, 482.353, 647.059, 717.647, 811.765, 952.941, 1023.529),
        (155.332, 305.646, 469.854, 643.542, 870.275, 1420.825, 1800.959),
    ),
    "precipitation": (
        (47.059, 188.235, 611.765, 1082.353, 1505.882, 4564.706, 4894.118),
        (6.849, 39.703, 256.887, 583.683, 1181.637, 2900.612, 4990.798),
    ),
    "precip_variability": (
        (21.176, 25.098, 39.216, 52.549, 64.314, 81.569, 83.922),
        (0.0, 7.586, 26.130, 45.783, 72.548, 110.545, 138.160),
    ),
}


def apply_climate_calibration(raw: np.ndarray, xs, ys) -> np.ndarray:
    """Piecewise-linear monotone remap with LINEAR EXTRAPOLATION at both ends.

    The extrapolation is the whole point and must not be dropped for a bare
    ``np.interp``. ``np.interp`` CLAMPS outside its anchors, which would pin
    every delivered temperature above the 99th-percentile anchor to a single
    output value -- a hard ceiling at 30.5 C. That is precisely the
    range-crushing this function exists to undo, so clamping would reintroduce
    the bug at the top of the distribution where the deserts live.

    Extending the end segments' slopes keeps the map strictly monotone over
    the whole real line, so ordering -- and therefore spatial pattern -- is
    preserved for extreme values too.
    """
    x = np.asarray(xs, dtype=np.float64)
    y = np.asarray(ys, dtype=np.float64)
    v = raw.astype(np.float64)
    out = np.interp(v, x, y)

    lo_slope = (y[1] - y[0]) / (x[1] - x[0])
    hi_slope = (y[-1] - y[-2]) / (x[-1] - x[-2])
    below = v < x[0]
    above = v > x[-1]
    out[below] = y[0] + (v[below] - x[0]) * lo_slope
    out[above] = y[-1] + (v[above] - x[-1]) * hi_slope
    return out


#: How far outside a channel's declared physical range a value may stray and
#: still be treated as model noise rather than breakage, as a fraction of the
#: channel's span. 5% of precipitation's [0, 12000] is 600 mm.
PHYSICAL_CLAMP_TOLERANCE = 0.05


def clamp_to_physical_range(
    raster_dict: dict[str, np.ndarray], mapping: dict[str, str]
) -> dict[str, int]:
    """Clamp small, physically-meaningless excursions; leave real breakage alone.

    WHY THIS IS NEEDED. Precipitation, seasonality and precip_variability are
    NON-NEGATIVE physical quantities, but the coarse model generates in a
    normalized latent space with nothing constraining its sign. On genuinely
    arid ground it undershoots zero: a real tile at (-4, 19) came back with
    precipitation in [-98.83, 361.55] -- an entire desert tile, max 362 mm/yr.
    ``validate_model_output`` refused it, which would have blocked generation
    of exactly the arid terrain the conditioning work exists to produce.

    Negative rainfall is not a prediction, it is noise about a floor the model
    does not know exists. Clamping it to that floor is the correct physical
    reading.

    WHY IT IS BOUNDED. A blanket clip would hide real breakage -- notably the
    failure mode ``adapt_raster_to_tile`` documents, where every climate plane
    saturated and four constant planes looked like "climate exists" while
    carrying no information. So excursions beyond
    ``PHYSICAL_CLAMP_TOLERANCE`` of the channel span are deliberately NOT
    clamped: they survive into ``validate_model_output`` and still fail there.

    Mutates ``raster_dict`` in place. Returns per-channel counts of clamped
    cells so callers can report rather than silently absorb.
    """
    clamped: dict[str, int] = {}
    for spec in EXPECTED_CHANNELS:
        key = mapping.get(spec.name, spec.name)
        arr = raster_dict.get(key)
        if arr is None:
            continue
        span = spec.max - spec.min
        slack = span * PHYSICAL_CLAMP_TOLERANCE
        low = (arr < spec.min) & (arr >= spec.min - slack)
        high = (arr > spec.max) & (arr <= spec.max + slack)
        n = int(low.sum()) + int(high.sum())
        if n:
            np.clip(arr, spec.min, spec.max, out=arr)
            clamped[spec.name] = n
    return clamped


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
    Climate: float32 in each channel's PHYSICAL WorldClim range (see
    EXPECTED_CHANNELS) -> normalized to [0, 1] by that range -> uint8 in
    [0, 255] (rounded, clipped), packed in the tile_codec channel order
    (temperature, seasonality, precipitation, precip_variability).

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
        # The model emits RAW WorldClim physical units, not [0, 1] -- see
        # EXPECTED_CHANNELS. Normalize per channel by its own physical range
        # before quantizing. The previous `raw * 255.0` silently saturated
        # every climate plane to 255 (bio_1 ~21 C, bio_12 ~630 mm all clip),
        # producing four identical constant planes that would have looked
        # like "climate exists" while carrying no information at all.
        spec = _CLIMATE_SPECS[name]
        # Undo the model's range compression BEFORE quantizing. Order matters:
        # calibrating after the uint8 step would work on 1/255-of-range
        # buckets and could not recover detail the quantizer had already
        # merged. See CLIMATE_CALIBRATION for why this is a fixed global curve
        # rather than a per-tile fit.
        if config.climate_calibration and name in CLIMATE_CALIBRATION:
            xs, ys = CLIMATE_CALIBRATION[name]
            raw = apply_climate_calibration(raw, xs, ys)
        span = spec.max - spec.min
        unit = (raw.astype(np.float64) - spec.min) / span
        climate[i] = np.clip(np.rint(unit * 255.0), 0, 255).astype(np.uint8)

    return Tile(seed=seed, x=x, y=y, scale=scale, elevation=elevation, climate=climate)


# ---------------------------------------------------------------------------
# Pinned bring-up config.
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class WorldShapeConfig:
    """The ``WorldPipeline`` constructor kwargs that decide what the world LOOKS
    LIKE — and, unlike the ``SamplerConfig`` this replaces, ones that are
    actually passed to the model.

    WHY THE OLD FIELD WENT. ``SamplerConfig`` carried ``steps``,
    ``guidance_scale`` and ``scheduler``, and its own docstring recorded that
    they "currently reach nothing": ``WorldPipeline.from_pretrained``/``bind``/
    ``get`` accept no sampler arguments, so the values in force were always
    upstream defaults. That made the identity wrong in both directions — editing
    ``steps`` rolled the id while producing byte-identical tiles, and the real
    settings were absent from it. The docstring asked for exactly one of two
    resolutions: wire them through, or delete them. There is nowhere to wire
    them to, so they are deleted, and replaced with the knobs that DO reach the
    model (``world_pipeline.py``'s ``__init__``, forwarded by
    ``from_pretrained``).

    Every field here changes generated tile bytes, so every field is hashed.

    Defaults are copied from ``WorldPipeline.__init__`` as of terrain-diffusion
    at 2026-07-25, so an unset config passes exactly what upstream would have
    defaulted to and generates what it generated before this class existed.
    That copy is the weak point — upstream could change a default and this would
    keep passing the old one — so ``test_world_shape_defaults_are_pinned``
    pins them, and ``terrain_diffusion_version`` is what records which upstream
    a tile set was actually built against.

    See ``terrain-service/docs/worldgen-levers.md`` for what each one does and
    what it is worth.
    """

    #: Per-channel Perlin frequency of the coarse SKETCH the model is
    #: conditioned on (elevation, temp, temp_std, precip, precip_cv). Index 0 is
    #: the landmass-scale knob: measured, 1.5 -> 0.4 takes inland reach from
    #: 123 km to 192 km and the largest landmass from 197k to 315k km2.
    frequency_mult: tuple[float, ...] = (1.5, 3.0, 3.0, 3.0, 3.0)
    #: Land/ocean ratio, via masking ocean pixels out of the elevation
    #: histogram. NOTE: upstream caches the quantile tables in
    #: data/global/synthetic_map_stats.json WITHOUT keying on this value, so it
    #: silently does nothing unless that file is deleted first. Hashed anyway —
    #: an identity that ignored it would be wrong the moment the cache is
    #: cleared. See docs/worldgen-levers.md §6.
    drop_water_pct: float = 0.5
    #: Per-channel conditioning SNR: how tightly the coarse model must obey the
    #: sketch. The analogue of tiff-export's --snr.
    cond_snr: tuple[float, ...] = (0.3, 0.1, 1.0, 0.1, 1.0)
    #: Pools the coarse output, compressing horizontal space.
    coarse_pooling: int = 1
    elev_coarse_pool_mode: str = "avg"
    p5_coarse_pool_mode: str = "avg"

    #: OROGRAPHIC PRECIPITATION. Upstream couples temperature to elevation via
    #: a real lapse rate but couples precipitation to NOTHING, so before this
    #: the two fields were independent Perlin draws and a desert could sit on
    #: the wet side of a range. This makes rain shadows a consequence of the
    #: terrain the model was already going to generate: precip is scaled by a
    #: windward-enhancement / lee-suppression factor computed from how much
    #: higher the upwind terrain is.
    #:
    #: MEASURED on the 256x256 sketch at seed 20260719: correlation between
    #: upwind barrier height and the precip multiplier is -0.734; mean
    #: multiplier is 0.493 behind a >600 m barrier against 1.393 with none.
    #: Land under 400 mm/yr goes 34.7% -> 40.4%; land over 2000 mm/yr goes
    #: 7.8% -> 9.1%. Both tails grow, which is the signature of redistribution
    #: rather than a global scale factor.
    #:
    #: Flat scalars rather than a dict because the dataclass is frozen and
    #: hashable, and because every one of them then shows up individually in
    #: the identity payload.
    orographic_enabled: bool = True
    #: Bearing the wind blows FROM. Sets a CONSISTENT prevailing direction; its
    #: absolute compass mapping through the pipeline's coordinate swaps is
    #: deliberately not claimed. See the note in synthetic_map.py.
    oro_wind_from_deg: float = 270.0
    #: Upwind probe distances as FRACTIONS OF THE ELEVATION BASE WAVELENGTH,
    #: so the physics stays scale-correct if frequency_mult[0] moves.
    oro_probe_wavelengths: tuple[float, ...] = (0.15, 0.30, 0.60, 1.20)
    oro_barrier_m: float = 1200.0
    oro_upslope_m: float = 600.0
    oro_shadow_strength: float = 0.75
    oro_enhance_strength: float = 0.60
    oro_sea_blend_m: float = 200.0

    #: ELEVATION TAIL STRETCH. Monotone gain applied to the elevation quantile
    #: table's above-sea-level knots, anchored at v=0 so coastlines never move:
    #: ``v' = v + (gain-1)*vmax*(v/vmax)^power``. It is the ONLY elevation-
    #: variance lever that exists -- ``seed`` picks a realization of a fixed
    #: process, and ``frequency_mult`` cannot change a marginal that quantile
    #: matching pins by construction.
    #:
    #: MEASURED on the coarse model, 64x64 cells (492 km) at seed 20260719,
    #: which also FALSIFIED the reason this lever was originally proposed. The
    #: claim was that the model compresses the elevation tails (table implies
    #: 22.6% of land above 1000 m, "delivered 1.4%"). It does not: at gain 1.0
    #: the table asks 22.2% and the model DELIVERS 24.3%. The 1.4% figure was
    #: local relief per 2 km WINDOW, a different quantity that was conflated
    #: with elevation. See docs/measurements/elevation-tails-2026-08-01.txt.
    #:
    #:   gain  delivered  %land>1km  %land>2km   p95      max
    #:   1.0              24.31%      5.29%     2025 m   4799 m
    #:   1.6              27.05%      8.07%     2406 m   7465 m
    #:   2.0              28.63%      9.34%     2665 m   8144 m
    #:
    #: 1.6 ships: it lifts peaks 4799 -> 7465 m and half again as much land
    #: above 2 km, while staying less far outside the model's training
    #: distribution than 2.0 (whose table max of 11.6 km comes back as 8.1 km,
    #: i.e. the model is visibly clipping it). The coarse tier is a 7.68 km
    #: cell MEAN, so in-game peak voxels sit above these numbers once the fine
    #: tier and bake add relief on top -- 7465 m here is Everest-class terrain
    #: in the world, not a 7465 m summit.
    elev_gain: float = 1.6
    elev_gain_power: float = 2.0

    def as_pipeline_kwargs(self) -> dict:
        """The exact kwargs to hand ``WorldPipeline.from_pretrained``.

        Lists, not tuples: upstream indexes and slices these, and a tuple would
        work by accident today and break on the first ``.append``-shaped change
        upstream makes. Tuples are used on the dataclass only so it stays
        hashable and frozen.
        """
        return {
            "frequency_mult": list(self.frequency_mult),
            "drop_water_pct": self.drop_water_pct,
            "cond_snr": list(self.cond_snr),
            "coarse_pooling": self.coarse_pooling,
            "elev_coarse_pool_mode": self.elev_coarse_pool_mode,
            "p5_coarse_pool_mode": self.p5_coarse_pool_mode,
            # None, not an empty dict: upstream treats None as "no orographic
            # term at all" and reproduces its pre-2026-08-01 output exactly,
            # which is what makes the old world reconstructible.
            "orographic": (
                {
                    "wind_from_deg": self.oro_wind_from_deg,
                    "probe_wavelengths": list(self.oro_probe_wavelengths),
                    "barrier_m": self.oro_barrier_m,
                    "upslope_m": self.oro_upslope_m,
                    "shadow_strength": self.oro_shadow_strength,
                    "enhance_strength": self.oro_enhance_strength,
                    "sea_blend_m": self.oro_sea_blend_m,
                }
                if self.orographic_enabled
                else None
            ),
            "elev_gain": self.elev_gain,
            "elev_gain_power": self.elev_gain_power,
        }


#: Bumped whenever the *shape* of the provider_id payload changes (fields
#: added/removed/renamed), so two schemes can never coincidentally collide.
#: v1 = the bring-up scheme (checkpoint_id in the id, no conditioning hash).
#: v2 = current: identity is content-addressed only (load path excluded),
#: conditioning data + tile wire format folded in.
#: v3 = the `sampler` field (hashed, never passed to anything) replaced by
#: `world_shape` (hashed AND passed). Rolls every provider_id; adopt an existing
#: cache with provider_id_override, which is exactly what it is for.
IDENTITY_SCHEMA_VERSION = 3

#: Placeholder meaning "the conditioning data behind this config has never
#: been hashed". Deliberately mirrors ``"UNPINNED"`` for the checkpoint: a
#: config carrying it is visibly marked in ``provider_id()`` and is refused
#: by ``verify_conditioning_digest`` before any real inference.
UNVERIFIED = "UNVERIFIED"

#: Directory ``terrain_diffusion.synthetic_map._compute_map_stats`` reads
#: from. It is RELATIVE and resolved from the process CWD by the upstream
#: package, so we resolve it the same way (see ``resolve_conditioning_root``)
#: and record only *relative* names in the digest manifest — an absolute path
#: in an identity is exactly bug #1 all over again.
DEFAULT_CONDITIONING_ROOT = "data/global"

#: The conditioning rasters whose CONTENT changes generated terrain: the
#: ETOPO relief raster built by ``tools/fetch_etopo.py`` (its bytes depend on
#: which NOAA product was reachable — the candidate list includes both a
#: ``_bed`` and a ``_surface`` variant, so divergence here is likely, not
#: theoretical) plus the four WorldClim 2.1 10-arc-minute bio rasters that
#: EXPECTED_CHANNELS is calibrated against (bio_1/4/12/15) and that
#: ``fetch_etopo.py`` uses as its reference grid.
#:
#: This is an explicit ALLOW-LIST, not "hash the whole directory", on
#: purpose: ``data/global`` also holds the multi-hundred-MB raw NOAA download
#: (``_etopo_source.tif``, an intermediate the pipeline never reads) and
#: whatever other bio rasters a given box happened to download. Hashing those
#: would make identity depend on incidental local junk — a false kMismatch,
#: which is the same class of bug as embedding the load path. The list itself
#: is a config field, so extending it rolls the provider_id honestly.
#: ``synthetic_map_stats.json`` is in the list even though it is DERIVED from
#: the five rasters above, and hashing a derived file alongside its inputs is
#: normally redundant. It is not redundant here, for two measured reasons:
#:
#:   1. It is a cache **keyed on nothing**. ``_load_stats_cache``
#:      (``synthetic_map.py:185-188``) returns it whenever the file exists,
#:      ignoring the ``frequency_mult`` and ``drop_water_pct`` it was built
#:      under — so it can disagree with this config's values and no code
#:      notices. Hashing the inputs tells you what the cache SHOULD hold;
#:      only hashing the file tells you what it DOES hold.
#:   2. The shipped copy was built from FAKE climate. ``_prep_stats.py``
#:      records that WorldClim was unreachable and hand-written latitude
#:      formulas were substituted for bio_1/4/12/15; the real rasters sat
#:      unused beside it. Rebuilt from the real rasters 2026-08-01, which
#:      moved precipitation's IQR 441 -> 851 mm and its p5 320 -> 39 mm --
#:      a different world under a byte-identical set of input rasters.
#:
#: It is also gitignored and untracked in the terrain-diffusion repo, so it
#: is exactly the kind of machine-local derived artifact that silently
#: diverges between boxes. That is an argument for hashing it, not against.
DEFAULT_CONDITIONING_FILES: tuple[str, ...] = (
    "etopo_10m.tif",
    "wc2.1_10m_bio_1.tif",
    "wc2.1_10m_bio_4.tif",
    "wc2.1_10m_bio_12.tif",
    "wc2.1_10m_bio_15.tif",
    "synthetic_map_stats.json",
)


#: MANUAL VERSION COUNTER for generation logic that is pure CODE and cannot
#: be hashed automatically. **Bump it in the same commit as any change to:**
#:
#:   * ``derive_tile_seed``'s canonical string, field order, digest slice or
#:     endianness — and the ``torch.manual_seed`` call that consumes it;
#:   * ``TerrainDiffusionBackend.generate_rasters``' tile-to-pixel axis
#:     mapping (``i1 = y*TILE_SIZE, j1 = x*TILE_SIZE`` — still marked
#:     ``# ASSUMPTION:`` and the single most likely edit in this file: if
#:     bring-up finds it transposed, the whole world transposes);
#:   * ``_get_native``'s fetch (it used to be ``_get_terrain_at_scale``, whose
#:     bilinear upsampling math was in scope here for the same reason — see
#:     v2 below);
#:   * ``adapt_raster_to_tile``'s conversion beyond the ranges already
#:     covered below, or ``_synthetic_stand_in``'s stand-in arithmetic.
#:
#: A counter is not as good as hashing the source, but hashing source text
#: would roll the id on comment edits and reformattings — a false kMismatch,
#: the same failure mode as bug #1. An explicit, documented counter puts the
#: decision where the judgement is.
#:
#: History:
#:   1 — bring-up.
#:   2 — deleted ``_get_terrain_at_scale``'s bilinear scale>1 branch. Nothing
#:       was ever generated at scale 8 so no cached tile changes meaning, but
#:       the sub-30 m slot now means "baked fine tier" instead of "upsampled
#:       coarse tile", and that is exactly the kind of reinterpretation this
#:       counter exists to make visible.
GENERATION_ALGORITHM_VERSION = 2


def _tile_format_fingerprint() -> str:
    """sha256 of everything OUTSIDE DiffusionConfig that still decides tile
    bytes.

    Three groups, none of them config fields — so before this they could all
    be edited without rolling ``provider_id``, silently changing tile bytes
    (or the cached byte FORMAT) under an unchanged identity:

      * wire geometry: ``TILE_SIZE``, ``CLIMATE_CHANNELS``, ``PIXEL_SIZE_MM``;
      * the encoded-tile container: ``tile_codec``'s ``MAGIC``, ``VERSION``
        and header struct. A codec bump is especially nasty without this —
        cache files are keyed only by provider_id, so the namespace would end
        up holding two mutually incompatible formats and ``decode`` would
        raise "unsupported tile version" on the old ones;
      * the adapter's quantization contract: ``EXPECTED_CHANNELS`` min/max
        (which ``adapt_raster_to_tile`` uses as the uint8 mapping range) and
        ``_CLIMATE_ORDER`` (the plane packing order);
      * **the geomorphic bake** — its version, stage order, geometry (tile
        size, scale, apron) and every physical constant, via
        ``bake.pipeline.bake_identity_payload()``. The fine tier is not a
        derived view of the coarse tile, it is new canonical world data that
        the client's collision reads, so a K change or an apron change is a
        different world in exactly the sense ``provider_id`` exists to
        express. Covering it here is what makes "a bake change yields a new
        world rather than a mixed one" true rather than a convention: the
        cache is keyed on ``provider_id`` alone, so without this a retuned
        bake would drop tiles into a namespace already holding tiles from the
        old one, and no consumer could tell them apart.

    plus ``GENERATION_ALGORITHM_VERSION`` for the logic that can only be
    tracked by hand (see that constant).

    The bake import is deliberately NOT wrapped in try/except. ``pipeline.py``
    imports nothing beyond stdlib+numpy at module scope precisely so this call
    works on a box with no numba/scipy; if it ever fails, the honest outcome
    is a loud error, because the alternative is a ``provider_id`` that
    silently stops covering the bake.
    """
    from .. import tile_codec

    payload = {
        "algorithm_version": GENERATION_ALGORITHM_VERSION,
        "tile_size": TILE_SIZE,
        "climate_channels": CLIMATE_CHANNELS,
        "pixel_size_mm": {str(k): v for k, v in sorted(PIXEL_SIZE_MM.items())},
        "codec_magic": tile_codec.MAGIC.decode("ascii"),
        "codec_version": tile_codec.VERSION,
        "codec_header": tile_codec._HEADER.format,
        "climate_order": list(_CLIMATE_ORDER),
        "expected_channels": [
            [c.name, c.dtype, c.min, c.max] for c in EXPECTED_CHANNELS
        ],
    }
    return hashlib.sha256(
        json.dumps(payload, sort_keys=True).encode("utf-8")
    ).hexdigest()


def _bake_fingerprint() -> str:
    """sha256 of the geomorphic bake's identity — version, stage order,
    geometry and every physical constant (``bake.pipeline``).

    SPLIT OUT OF ``_tile_format_fingerprint`` 2026-08-01, and the reason is
    the whole point of this function existing separately.

    The bake used to be folded into ``provider_id`` alongside the wire format,
    on a correct argument: the fine tier is not a derived view of the coarse
    tile, it is new canonical world data that the client's collision reads, so
    a K change or an apron change really is a different world. But
    ``provider_id`` keys the WHOLE namespace, coarse tiles included — and a
    coarse tile does not depend on the bake in any way. A bake-only tuning
    change therefore discarded coarse tiles that cost ~22.5 s of GPU each and,
    on a box with a CPU-only torch and an AMD GPU, cannot be regenerated at
    all. Three orphaned cache generations on this machine, one of them tagged
    BROKEN-DO-NOT-USE, are what that policy actually produced.

    So the namespaces split by what each artifact DEPENDS ON:

        coarse (s1)          provider_id           inference identity only
        fine (s16), flow     fine_provider_id      inference + bake identity

    Retuning the bake now re-keys only the artifacts the bake produced. The
    guarantee that made folding it in right — "a bake change yields a new
    world rather than a mixed one" — is unchanged, because every artifact the
    bake touches still moves to a fresh namespace together.

    NO CLIENT PATH GRAMMAR CHANGES, which is why this shape was chosen over
    adding a path segment: the client already takes the fine tier's provider
    id as its own parameter (``-VoxelFineTileProviderId`` /
    ``DefaultFineTileProviderId``), independent of the coarse tile directory,
    and ``FVoxelFineTileStreamer`` validates every tile's stamp against it.
    Two ids in config, zero C++ changes, and a mismatch is still refused and
    counted rather than trusted.

    The import stays unwrapped for the same reason it always was: ``pipeline``
    imports nothing beyond stdlib+numpy at module scope so this works on a box
    with no numba/scipy, and a silent fallback would mean an id that stopped
    covering the bake.

    TWO PAYLOADS SINCE bake_ver 9, and the pairing is the point.
    ``bake_identity_payload`` is the TERRAIN half -- what decides the ground --
    and ``product_identity_payload`` is the PRODUCT half: which sections a tile
    carries and the constants that fill them. Both are folded in, so content
    addressing keeps meaning what it says: a tile whose water plane differs
    never shares an id with one whose does not. See the TERRAIN_VERSION /
    BAKE_VERSION note in ``bake/pipeline.py`` for why they are separate
    counters at all.

    Including the product half is what makes a water retune SAFE rather than
    silent -- ``BakeConstants.as_payload`` is a whitelist, so a water constant
    that fed no identity would change written bytes under an unchanged id and
    the namespace would end up holding two mutually incompatible formats. That
    is exactly the failure this module's own ``_tile_format_fingerprint``
    docstring calls "especially nasty" about a codec bump.

    What it does NOT cost: water APPEARANCE -- translucency, colour, the depth
    cue, the foam channel -- is client-side material work that touches no baked
    byte, so the retuning done most often rolls nothing here.
    """
    from ..bake.pipeline import bake_identity_payload, product_identity_payload

    return hashlib.sha256(
        json.dumps(
            {"bake": bake_identity_payload(),
             "product": product_identity_payload()},
            sort_keys=True,
        ).encode("utf-8")
    ).hexdigest()


def fine_id_for(provider_id: str) -> str:
    """The bake-derived namespace belonging to a coarse ``provider_id``.

    ONE function rather than the same f-string in three places, because the
    three places are ``DiffusionConfig.fine_provider_id``,
    ``DiffusionProvider.__init__`` (which must inherit the ``-dryrun-`` tag its
    ``provider_id`` already carries) and ``SyntheticProvider``. A suffix that
    drifted between them would put the fine tier and the flow pyramid in
    different namespaces while every test still passed.

    Suffixing rather than re-hashing is deliberate: the coarse namespace stays
    readable straight off the directory name, and the UNPINNED /
    UNVERIFIEDDATA / dryrun markers that ``provider_id`` inserts in plain text
    survive into the fine id, so a stray fine cache is still self-describing.
    """
    return f"{provider_id}-b{_bake_fingerprint()[:8]}"


@dataclass(frozen=True)
class DiffusionConfig:
    """The pinned terrain-diffusion bring-up config.

    Everything a GPU bring-up session needs to pin down before generating
    canonical tiles: which checkpoint (by CONTENT, never by location), which
    conditioning data, which sampler settings, which tile scale it targets,
    and how the model's raw channel names map onto ours. Per doctrine §2.3
    ("Determinism boundary"), diffusion output is not bit-deterministic
    across GPUs/versions, so tiles are generated once and distributed as
    data — this config is exactly the set of knobs that must be pinned (and
    folded into ``provider_id()``) so a cache can never silently mix tiles
    from two different configs.

    Identity rule (see ``provider_id``): a field belongs in the id if and
    only if changing it can change generated bytes. ``checkpoint_id`` is a
    load LOCATION and therefore does not.
    """

    #: WHERE to load the checkpoint from — a local filesystem path to a
    #: pre-downloaded ``WorldPipeline`` snapshot (see
    #: ``TerrainDiffusionBackend._load_pipeline``). This is deliberately NOT
    #: part of ``provider_id()``: the same checkpoint mounted at
    #: ``/workspace/ckpt/...`` on a pod and at ``D:\ckpt\...`` on a laptop is
    #: the same checkpoint, and stamping the path into the id made
    #: ``EditLog::checkProvider()`` report kMismatch — refusing a replay — on
    #: a world that was byte-for-byte fine. Identity comes from
    #: ``checkpoint_sha256`` + ``checkpoint_label``.
    checkpoint_id: str = "./checkpoint"
    #: Human-readable name for the checkpoint, e.g. "terrain-diffusion-30m".
    #: Purely cosmetic (it makes cache directories and edit-log stamps
    #: legible) but it IS hashed, so two runs cannot disagree about the label
    #: while claiming one identity. Must not be a path — ``__post_init__``
    #: rejects separators, so the old "put the mount point here" habit fails
    #: loudly instead of silently re-introducing bug #1.
    checkpoint_label: str = "unlabeled"
    #: sha256 of the checkpoint file/weights, pinned once known. Prevents a
    #: silent checkpoint swap (same id, different weights) from mixing into
    #: an existing cache under the same provider_id. THIS, not the path, is
    #: what makes the identity content-addressed.
    checkpoint_sha256: str = "UNPINNED"
    #: sha256 manifest digest of the conditioning rasters
    #: (``compute_conditioning_digest``). ``WorldPipeline`` does not generate
    #: from weights alone — it conditions on the WorldClim bio rasters and
    #: ``data/global/etopo_10m.tif`` via ``synthetic_map._compute_map_stats``
    #: — so two boxes with different ETOPO/WorldClim bytes produce different
    #: terrain. Without this in the id that divergence is invisible to
    #: ``EditLog::checkProvider()``, which is precisely the failure it exists
    #: to catch. ``UNVERIFIED`` until a bring-up session pins it.
    conditioning_digest: str = UNVERIFIED
    #: Which files ``conditioning_digest`` covers (relative to the
    #: conditioning root). A config field so extending coverage rolls the id.
    conditioning_files: tuple[str, ...] = DEFAULT_CONDITIONING_FILES
    #: Version/commit of the terrain-diffusion package itself, when known.
    #: Not bit-determinism (doctrine §2.3 says that is unattainable), but a
    #: package upgrade can change output STRUCTURALLY, and that should not
    #: hide under an unchanged id. "UNRECORDED" if a bring-up did not note it.
    #:
    #: Now records BOTH halves of what is actually installed: the pinned
    #: upstream commit AND sha256[0:12] of our worldgen patch
    #: (terrain-service/patches/terrain-diffusion-worldgen.patch, which adds
    #: the orographic rain shadow and the elevation tail stretch). Upstream
    #: alone would be a half-truth -- two boxes on the same commit, one
    #: patched and one not, generate different worlds. bootstrap_pod.sh pins
    #: the same commit at its TD_COMMIT; bump both together or neither.
    terrain_diffusion_version: str = "82a0431+worldgen.c55a6382c524"
    #: Apply CLIMATE_CALIBRATION to model output before quantization. NOT a
    #: WorldShapeConfig field on purpose: as_pipeline_kwargs() is documented as
    #: "exactly what the model receives", and this is OUR post-processing of
    #: what comes back. It is hashed separately into provider_id below.
    #:
    #: Set False to reproduce the raw, uncalibrated model climate -- which
    #: contains no hot-and-arid land anywhere in the world, so no desert.
    climate_calibration: bool = True
    #: The WorldPipeline kwargs that shape the world. Replaces the old
    #: ``sampler`` field, which was hashed but never passed to anything — see
    #: WorldShapeConfig's docstring.
    world_shape: WorldShapeConfig = field(default_factory=WorldShapeConfig)
    #: Tile pixel scale this config is calibrated for (tile_codec.PIXEL_SIZE_MM
    #: key: 1 => 30m/px, 8 => 3.75m/px supersampled). Must match the `scale`
    #: argument the provider is actually called with.
    scale: int = 1
    #: Semantic channel name -> raster dict key the model emits. Identity
    #: by default; edit at bring-up if the real checkpoint's raster keys
    #: differ from ours (config change, not code change — see module
    #: docstring and adapt_raster_to_tile).
    channel_mapping: dict[str, str] = field(
        default_factory=lambda: dict(DEFAULT_CHANNEL_MAPPING)
    )
    #: COMPATIBILITY ESCAPE HATCH. When set, ``provider_id()`` returns this
    #: string verbatim, ignoring everything above. Its only sanctioned use is
    #: adopting a cache namespace that already exists on disk — e.g. resuming
    #: or serving the tiles generated under the v1 (pre-fix) id. It defeats
    #: every guarantee in this class, so it is explicit, opt-in, never
    #: defaulted, and reported as-is in the cache path and edit-log stamp.
    provider_id_override: str | None = None

    def __post_init__(self) -> None:
        if self.scale not in PIXEL_SIZE_MM:
            raise ValueError(f"unsupported scale {self.scale}, must be one of {sorted(PIXEL_SIZE_MM)}")
        missing = {c.name for c in EXPECTED_CHANNELS} - set(self.channel_mapping)
        if missing:
            raise ValueError(f"channel_mapping missing entries for: {sorted(missing)}")
        label = self.checkpoint_label
        if not label or label.strip() != label:
            raise ValueError(
                f"checkpoint_label must be a non-empty, unpadded name, got {label!r}"
            )
        bad = set(label) & set("/\\:")
        if bad or label in (".", ".."):
            raise ValueError(
                f"checkpoint_label looks like a filesystem path ({label!r}): it is a "
                "human-readable NAME and is hashed into provider_id, which must never "
                "depend on where the bytes are mounted. Put the load path in "
                "checkpoint_id (which is deliberately excluded from the identity) and "
                "give the label something like 'terrain-diffusion-30m'."
            )
        # `frozen=True` stops attribute rebinding but NOT
        # `config.channel_mapping["temperature"] = "t2m"`, and the mapping is
        # read live on every generate() while provider_id was snapshotted at
        # construction -- one mutation would repack the climate planes under
        # an already-published identity. Freeze the contents too.
        object.__setattr__(
            self, "channel_mapping", MappingProxyType(dict(self.channel_mapping))
        )
        object.__setattr__(self, "conditioning_files", tuple(self.conditioning_files))
        if not self.conditioning_files:
            raise ValueError(
                "conditioning_files must not be empty: an identity that covers no "
                "conditioning data cannot detect the ETOPO/WorldClim divergence it "
                "exists to detect"
            )

    def provider_id(self) -> str:
        """Stable identity+version string for the cache key (TileProvider
        contract), and the value stamped into the edit log that
        ``EditLog::checkProvider()`` compares to decide whether replaying a
        saved world against a tile set is safe.

        Shape: ``terrain-diffusion-<label>[-UNPINNED][-UNVERIFIEDDATA]-<16 hex>``

        The hash covers every input that can change generated bytes — model
        content (``checkpoint_sha256``), conditioning-data content
        (``conditioning_digest`` + the file list it covers), the
        terrain-diffusion version, sampler settings, scale, channel mapping,
        the human label, the tile wire/quantization format
        (``_tile_format_fingerprint``), and the schema version — and NOTHING
        about where any of those bytes live on disk. Same content, different
        mount point => same id => ``kMatch``, as it must be.

        Unpinned/unverified configs are marked IN PLAIN TEXT rather than
        being allowed to look like a normal identity, so a stray cache
        directory or edit-log stamp is self-describing. (Real inference is
        refused outright by ``verify_checkpoint_sha256`` /
        ``verify_conditioning_digest``; the marker is what protects anything
        already written by a dry-run or a mis-wired config.)
        """
        if self.provider_id_override is not None:
            return self.provider_id_override
        payload = {
            "identity_schema": IDENTITY_SCHEMA_VERSION,
            # NOTE: checkpoint_id (the load path) is intentionally absent.
            "checkpoint_label": self.checkpoint_label,
            "checkpoint_sha256": self.checkpoint_sha256,
            "conditioning_digest": self.conditioning_digest,
            "conditioning_files": sorted(self.conditioning_files),
            "terrain_diffusion_version": self.terrain_diffusion_version,
            # Everything that shapes the world AND is actually passed to
            # WorldPipeline. Serialized from as_pipeline_kwargs() rather than
            # field-by-field so the hash covers exactly what the model receives:
            # a kwarg that stops being passed can no longer sit in the identity
            # pretending to matter, which is precisely how `sampler` went wrong.
            "world_shape": self.world_shape.as_pipeline_kwargs(),
            # Post-processing of model output, so it does not live in
            # world_shape. The CURVES are hashed, not just the on/off flag:
            # re-fitting them silently changes every tile's climate, which is
            # exactly the kind of change that must not hide under an unchanged
            # id -- the same lesson as synthetic_map_stats.json.
            "climate_calibration": (
                {k: [list(v[0]), list(v[1])] for k, v in sorted(CLIMATE_CALIBRATION.items())}
                if self.climate_calibration
                else None
            ),
            "scale": self.scale,
            "channel_mapping": dict(self.channel_mapping),
            "tile_format": _tile_format_fingerprint(),
        }
        digest = hashlib.sha256(
            json.dumps(payload, sort_keys=True).encode("utf-8")
        ).hexdigest()[:16]
        parts = ["terrain-diffusion", self.checkpoint_label]
        if self.checkpoint_sha256 == "UNPINNED":
            parts.append("UNPINNED")
        if self.conditioning_digest == UNVERIFIED:
            parts.append("UNVERIFIEDDATA")
        parts.append(digest)
        return "-".join(parts)

    def fine_provider_id(self) -> str:
        """Identity for BAKE-DERIVED artifacts: the fine tier and the flow
        superblocks.

        ``provider_id()`` above covers inference only, so coarse tiles survive
        a bake retune (see ``_bake_fingerprint`` for why that split exists and
        what the old policy cost). Everything the bake produced keys on this
        instead: the same inference identity plus the bake's own digest, so a
        constant change moves the fine tier and the flow pyramid to a fresh
        namespace TOGETHER, and neither can ever mix with the other's
        generation.

        Formed by suffixing ``provider_id()`` rather than hashing a combined
        payload, deliberately: the coarse namespace it derives from is then
        readable straight off the directory name, so a human looking at a cache
        root can see which fine generations belong to which coarse tiles
        without running anything. It also keeps the "unpinned/unverified"
        markers ``provider_id`` inserts in plain text — a stray fine cache
        stays self-describing.

        8 hex digits of the bake digest, not 16: this suffixes an id that is
        already 16, the space being distinguished is bake configurations of one
        project rather than all content everywhere, and a directory name that
        no one can read at a glance is its own kind of hazard. Collisions are
        change-detection failures, not correctness failures — the fine tile's
        stamp is validated against this same string by the client.
        """
        return fine_id_for(self.provider_id())


# ---------------------------------------------------------------------------
# Checkpoint verification (pure, torch-free -- unit-testable with temp files).
# ---------------------------------------------------------------------------


def _sha256_of_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _sha256_of_checkpoint_path(path: "str | Path") -> str:
    """sha256 of a checkpoint path, file OR directory.

    A real terrain-diffusion checkpoint is NOT one file: ``WorldPipeline``
    is a diffusers ``ConfigMixin``/``ModelMixin`` pipeline whose
    ``from_pretrained`` loads a top-level ``config.json`` plus three
    submodels (``coarse_model/``, ``base_model/``, ``decoder_model/``),
    each its own ``config.json`` + ``*.safetensors`` (confirmed by reading
    ``terrain_diffusion/inference/world_pipeline.py``'s ``from_pretrained``
    and ``terrain_diffusion/models/edm_unet.py``'s ``EDMUnet2D(ModelMixin,
    ConfigMixin)``). So a directory is hashed as a canonical manifest --
    sha256 of every file's ``relative/path:sha256`` line, sorted by path --
    which lets one sha256 pin an entire multi-file snapshot. A single file
    path (e.g. bring-up/tests using one checkpoint blob) is hashed directly.
    """
    p = Path(path)
    if p.is_file():
        return _sha256_of_file(p)
    if p.is_dir():
        lines = [
            f"{f.relative_to(p).as_posix()}:{_sha256_of_file(f)}"
            for f in sorted(p.rglob("*"))
            if f.is_file()
        ]
        return hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest()
    raise FileNotFoundError(f"checkpoint path does not exist: {p}")


def verify_checkpoint_sha256(path: "str | Path", expected: str) -> None:
    """Verify a checkpoint file/directory's sha256 against ``expected``.

    MUST be called before any load/inference call (doctrine: a silent
    checkpoint swap under an unchanged ``provider_id`` would poison the
    content-addressed tile cache with tiles from two different models).

    Raises ``ValueError`` if ``expected`` is the ``"UNPINNED"`` placeholder
    (refuses to run against an unpinned checkpoint) or if the computed hash
    does not match -- the message names both the expected and actual hash.
    Returns ``None`` (does not raise) when the hash matches.
    """
    if expected == "UNPINNED":
        raise ValueError(
            "checkpoint_sha256 is 'UNPINNED' -- refusing to run inference "
            "against an unpinned checkpoint (doctrine: a silent checkpoint "
            "swap under an unchanged provider_id would poison the cache). "
            "Pin DiffusionConfig.checkpoint_sha256 at GPU bring-up -- see "
            "terrain-service/docs/diffusion-bringup.md step 3."
        )
    actual = _sha256_of_checkpoint_path(path)
    if actual != expected:
        raise ValueError(
            f"checkpoint sha256 mismatch for {str(path)!r}: expected "
            f"{expected!r}, got {actual!r} -- refusing to load. If this "
            "checkpoint is intentionally different, mint a new "
            "DiffusionConfig with the new sha256 (this rolls provider_id, "
            "so old and new tiles never collide in the cache)."
        )


# ---------------------------------------------------------------------------
# Conditioning-data verification (pure, torch-free -- unit-testable with
# temp files, exactly like the checkpoint helpers above).
#
# WHY this exists: WorldPipeline does NOT generate from model weights alone.
# At bring-up it downloads the WorldClim 2.1 10-arc-minute bio rasters and
# reads data/global/etopo_10m.tif; synthetic_map._compute_map_stats derives
# statistics from them that CONDITION generation. Those files are generation
# inputs with none of the checkpoint's protections: etopo_10m.tif used to be
# BUILT per box by tools/fetch_etopo.py, resampling whichever NOAA product was
# reachable (its candidate list spanned a _bed and a _surface variant), so two
# boxes easily held different bytes. Before this, that produced different
# terrain under an IDENTICAL provider_id -- silently, into one cache namespace,
# and stamped identically into edit logs, so EditLog::checkProvider() could not
# see the very divergence it exists to refuse.
#
# Since 2026-08-02 the six files are PINNED BYTES obtained by
# tools/fetch_conditioning.py and verified against
# data/conditioning-artifacts.json, because the canonical etopo_10m.tif turned
# out never to have been a build output at all and could not be rebuilt by any
# settings -- see terrain_service/conditioning_artifacts.py. This digest is
# still the thing that decides identity; the pins are how a second box gets
# into a position to compute the same one.
# ---------------------------------------------------------------------------


class ConditioningDataMissing(FileNotFoundError):
    """Raised when the conditioning rasters an identity must cover are absent.

    A provider that cannot see its conditioning data must REFUSE to claim an
    identity rather than invent one: inventing it is what would let two
    materially different generation setups share a cache namespace.
    """

    def __init__(self, root: Path, missing: list[str]) -> None:
        self.root = root
        self.missing = missing
        super().__init__(
            f"conditioning data missing under {str(root)!r}: {missing} -- refusing "
            "to compute a conditioning digest (and therefore refusing to claim a "
            "provider identity) for data this process cannot see. These rasters "
            "condition generation via terrain_diffusion's synthetic_map."
            "_compute_map_stats. Run `python tools/fetch_conditioning.py` from "
            "terrain-service/ to obtain the PINNED bytes -- it verifies every "
            "file's sha256 and fails rather than substituting -- or point "
            "TERRAIN_CONDITIONING_ROOT at the directory that has them."
        )


def resolve_conditioning_root(root: "str | Path | None" = None) -> Path:
    """Resolve the conditioning-data directory the way upstream does.

    ``synthetic_map._compute_map_stats`` opens the RELATIVE path
    ``data/global/etopo_10m.tif``, i.e. resolved against the process CWD, so
    the effective location depends on where the worker was launched from.
    This resolves the same way (``TERRAIN_CONDITIONING_ROOT`` env override >
    explicit argument > ``TERRAIN_CONDITIONING_ROOT`` env >
    ``DEFAULT_CONDITIONING_ROOT`` relative to CWD) so the digest describes
    the files the model will ACTUALLY read.

    The resolved absolute path is used only to open files; it never enters
    the digest -- the manifest records relative names only (bug #1's lesson).
    """
    chosen = root or os.environ.get("TERRAIN_CONDITIONING_ROOT") or DEFAULT_CONDITIONING_ROOT
    return Path(chosen).resolve()


def compute_conditioning_digest(
    files: "tuple[str, ...] | list[str]" = DEFAULT_CONDITIONING_FILES,
    root: "str | Path | None" = None,
) -> str:
    """sha256 manifest digest over the CONTENT of the conditioning rasters.

    Hashes file BYTES (via the same ``relative/path:sha256`` sorted-manifest
    construction ``_sha256_of_checkpoint_path`` uses for a checkpoint tree),
    not a metadata manifest. Names+sizes+mtimes were rejected outright:
    mtimes differ on every fresh download, and size is not a content check --
    ETOPO's ``_bed`` and ``_surface`` variants are the same grid, dtype and
    compression family and can land within bytes of each other while
    describing different planets' worth of bathymetry. Bytes are the only
    answer that actually detects the divergence we are trying to detect.

    Cost is not a concern: these are ~5-20 MB rasters read once per process,
    against a 22.5 s per-tile generation cost.

    Raises ``ConditioningDataMissing`` if any listed file is absent -- see
    that exception for why refusing beats inventing.
    """
    digests = conditioning_file_digests(files, root)
    lines = [f"{n}:{digests[n]}" for n in sorted(files)]
    return hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest()


def conditioning_file_digests(
    files: "tuple[str, ...] | list[str]" = DEFAULT_CONDITIONING_FILES,
    root: "str | Path | None" = None,
) -> dict[str, str]:
    """Per-file sha256 of the conditioning rasters: ``{relative name: sha256}``.

    The same bytes ``compute_conditioning_digest`` folds into one number, kept
    ITEMISED. The digest alone answers "is this the same conditioning data?";
    it cannot answer "which file moved?", and on 2026-08-03 that second
    question was the whole investigation -- four of the six files matched
    across machines and the two that did not were exactly the two
    ``tools/bootstrap_pod.sh`` BUILDS rather than downloads
    (``etopo_10m.tif``, and the ``synthetic_map_stats.json`` derived from it).
    Recovering that took a manual hash-by-hash comparison against a pod that
    no longer exists. ``world_manifest`` records this mapping with every world
    so the next such comparison is a diff of two files already on disk.

    Raises ``ConditioningDataMissing`` for the same reason the digest does:
    inventing an identity for data this process cannot see is the failure
    mode, not the safety net.
    """
    resolved = resolve_conditioning_root(root)
    names = sorted(set(files))
    missing = [n for n in names if not (resolved / n).is_file()]
    if missing:
        raise ConditioningDataMissing(resolved, missing)
    return {n: _sha256_of_file(resolved / n) for n in names}


def verify_conditioning_digest(
    config: "DiffusionConfig", root: "str | Path | None" = None
) -> None:
    """Verify on-disk conditioning data against ``config.conditioning_digest``.

    The exact counterpart of ``verify_checkpoint_sha256``, and called from
    the same place (before any load/inference). Raises ``ValueError`` if the
    config is ``UNVERIFIED`` or if the data on this box does not match what
    the identity claims; ``ConditioningDataMissing`` if the data is absent.
    """
    if config.conditioning_digest == UNVERIFIED:
        raise ValueError(
            "conditioning_digest is 'UNVERIFIED' -- refusing to run inference "
            "against unhashed conditioning data. WorldPipeline conditions on the "
            "WorldClim bio rasters and data/global/etopo_10m.tif, so two boxes "
            "with different copies generate different terrain; leaving this "
            "unpinned would stamp both into edit logs under one identity and "
            "defeat EditLog::checkProvider(). Pin it with "
            "compute_conditioning_digest() -- see docs/diffusion-bringup.md step 3."
        )
    actual = compute_conditioning_digest(config.conditioning_files, root)
    if actual != config.conditioning_digest:
        raise ValueError(
            f"conditioning data mismatch under {str(resolve_conditioning_root(root))!r}: "
            f"config pins {config.conditioning_digest!r}, this box has {actual!r} -- "
            "refusing to generate. Run `python tools/fetch_conditioning.py "
            "--verify-only` from terrain-service/: it names WHICH of the six files "
            "differs and prints both hashes, which is the fact worth having (knowing "
            "the digest moved is not). The usual cause is a locally BUILT "
            "etopo_10m.tif or a synthetic_map_stats.json derived from one -- neither "
            "reproduces the pinned bytes. Fix the data, or mint a new DiffusionConfig "
            "with the new digest (this rolls provider_id, so the two tile sets never "
            "collide). Do NOT use provider_id_override to force the old namespace."
        )


def derive_tile_seed(seed: int, x: int, y: int, scale: int) -> int:
    """Deterministic per-tile RNG seed, hashed from (seed, x, y, scale).

    Pure Python (sha256 of a canonical string, no numpy/torch) so it is
    trivially unit-testable and carries no import-time weight. Used as
    defense-in-depth: ``TerrainDiffusionBackend`` seeds torch's global RNG
    from this value immediately before each inference call, layered on top
    of ``WorldPipeline``'s own internal per-tile seeding (see that class's
    docstring for why both exist).

    Per doctrine (module docstring, §2.3): diffusion output is NOT
    bit-deterministic across GPUs/torch versions, so this does NOT promise
    cross-machine reproducibility -- only that reruns on the SAME machine
    (same GPU, same torch/driver versions) reproduce the same tile. That is
    exactly why tiles are generated once and distributed as cached data
    rather than regenerated-and-compared.
    """
    canonical = f"terrain-diffusion-tile-seed:{int(seed)}:{int(x)}:{int(y)}:{int(scale)}"
    digest = hashlib.sha256(canonical.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big")


# ---------------------------------------------------------------------------
# Model backend protocol + the real terrain-diffusion backend.
#
# Neither this protocol nor TerrainDiffusionBackend's __init__ import torch
# or terrain-diffusion -- only TerrainDiffusionBackend's methods do, lazily,
# so `import terrain_service.providers.diffusion` works on machines with no
# GPU/torch installed (see tests/test_diffusion.py's import test).
# ---------------------------------------------------------------------------


class ModelBackend(Protocol):
    """What DiffusionProvider needs from a model backend (real or fake)."""

    def generate_rasters(
        self, seed: int, x: int, y: int, scale: int
    ) -> dict[str, np.ndarray]:
        """Return RAW UNQUANTIZED float32 rasters at (TILE_SIZE, TILE_SIZE),
        keyed by the values of ``DiffusionConfig.channel_mapping`` -- the
        same contract ``validate_model_output``/``adapt_raster_to_tile``
        expect from any real model call."""
        ...


class TerrainDiffusionBackend:
    """Real terrain-diffusion inference backend. Needs a CUDA machine; see
    ``terrain-service/docs/diffusion-bringup.md`` for the bring-up runbook.

    Grounded in the actual xandergos/terrain-diffusion source (fetched and
    read at bring-up-research time -- README.md, API_README.md,
    ``terrain_diffusion/inference/world_pipeline.py``,
    ``terrain_diffusion/inference/api.py``,
    ``terrain_diffusion/common/model_utils.py``). Facts this class relies
    on, all confirmed by reading that source (not guessed):

      * Checkpoints are diffusers-style (``ModelMixin``/``ConfigMixin``)
        submodels published on HuggingFace. ``WorldPipeline.from_pretrained
        (pretrained_model_name_or_path, token=None, **kwargs)`` loads a
        pipeline-level ``config.json``, then three submodels via
        ``EDMUnet2D.from_pretrained(path, subfolder="coarse_model" /
        "base_model" / "decoder_model")``. There is NO single checkpoint
        *file* -- it's a directory tree (local) or an HF repo (remote) of
        three submodel subfolders, each with its own ``config.json`` +
        ``*.safetensors``. See ``_sha256_of_checkpoint_path`` for how this
        class still pins one sha256 over that whole tree.
      * ``WorldPipeline.get(i1, j1, i2, j2, with_climate=True)`` returns
        ``{'elev': (H, W) float torch.Tensor, meters; 'climate': (5, H, W)
        float torch.Tensor}``. ``i1, j1, i2, j2`` are pixel-space
        bounding-box coordinates -- matches ``tile_codec.py``'s
        ``[x*TILE_SIZE, (x+1)*TILE_SIZE)`` convention directly, no unit
        conversion needed.
      * Climate channel order (confirmed from ``WorldPipeline.
        _compute_climate``'s ``torch.stack([temp_realistic, coarse_up[3],
        coarse_up[4], coarse_up[5], beta_up[0]])`` and ``inference/api.py``'s
        ``_binary_response`` docstring "channels: temp, t_season, precip,
        p_cv"): index 0 = temperature, 1 = seasonality, 2 = precipitation,
        3 = precip_variability (index 4 = an internal beta coefficient, not
        exposed/needed). This is EXACTLY ``_CLIMATE_ORDER`` below, so
        ``DEFAULT_CHANNEL_MAPPING`` needs no reshuffling for the real model.
      * One ``WorldPipeline`` instance holds ONE world ``seed`` (constructor
        arg / ``change_seed()``); different ``(x, y)`` queries into the
        SAME seeded pipeline are what give seamless infinite terrain (via
        the ``infinite-tensor`` tile store + internal per-tile hashing) --
        ``(x, y)`` are NOT independent per-tile seeds in the real API.

    ASSUMPTIONs below (things the source didn't settle and a GPU bring-up
    session must confirm) are marked inline with ``# ASSUMPTION:``.
    """

    def __init__(self, config: DiffusionConfig) -> None:
        self.config = config
        self._pipeline = None  # lazily constructed by _load_pipeline()
        self._bound_seed: int | None = None

    def _load_pipeline(self):
        try:
            import torch
        except ImportError as exc:
            raise RuntimeError(
                "terrain-diffusion backend needs torch installed and a CUDA "
                "machine for real inference -- see "
                "terrain-service/docs/diffusion-bringup.md. Pass "
                "dry_run=True, or inject model_backend=<fake>, to exercise "
                "the surrounding plumbing without a GPU."
            ) from exc
        try:
            from terrain_diffusion.inference.world_pipeline import WorldPipeline
        except ImportError as exc:
            raise RuntimeError(
                "the terrain-diffusion package is not installed -- see "
                "terrain-service/docs/diffusion-bringup.md step 2."
            ) from exc

        # THE INSTALLED PIPELINE MUST ACTUALLY ACCEPT OUR WORLD-SHAPING KWARGS.
        #
        # Upstream's WorldPipeline.__init__ ends in `**deprecated_kwargs`,
        # which is read exactly once and only for `histogram_raw`. So against
        # an UNPATCHED upstream, `orographic=` and `elev_gain=` are accepted,
        # discarded, and never mentioned again: tiles generate normally with no
        # rain shadow and unstretched relief, while provider_id() -- which
        # hashes as_pipeline_kwargs() wholesale -- stamps them as having both.
        # A silently mislabeled tile set is unrecoverable after the fact; there
        # is nothing in the output that distinguishes it.
        #
        # from_pretrained splats a plain dict (`cls(**config)`) with no key
        # filtering, so nothing upstream will ever raise on our behalf. This is
        # the only place that can catch it. Checked by signature rather than by
        # version string because the version string is what would be wrong.
        import inspect

        params = inspect.signature(WorldPipeline.__init__).parameters
        required = [k for k in self.config.world_shape.as_pipeline_kwargs()
                    if k not in params]
        if required:
            raise RuntimeError(
                f"the installed terrain-diffusion does not accept "
                f"{sorted(required)} -- these would be swallowed by its "
                f"**deprecated_kwargs and SILENTLY IGNORED, producing a world "
                f"that does not match the provider_id this config would stamp "
                f"on it.\n"
                f"Apply terrain-service/patches/terrain-diffusion-worldgen.patch "
                f"to the terrain-diffusion checkout (bootstrap_pod.sh does this "
                f"automatically for a fresh clone; a checkout stamped as already "
                f"cloned by an older bootstrap will NOT have it -- delete the "
                f"clone stamp and re-run).\n"
                f"To generate the pre-patch world deliberately, set the "
                f"corresponding WorldShapeConfig fields to their neutral values "
                f"(orographic_enabled=False, elev_gain=1.0) so the identity "
                f"stops claiming them."
            )

        # ASSUMPTION: checkpoint_id is a LOCAL filesystem path to a
        # pre-downloaded pipeline snapshot (e.g. via `huggingface_hub.
        # snapshot_download(repo_id, local_dir=...)` or `WorldPipeline(...).
        # save_pretrained(...)` at bring-up time) -- NOT a bare HuggingFace
        # repo id string. Doctrine requires sha256 verification BEFORE any
        # load/inference call; a bare repo id can't be hashed before
        # terrain-diffusion downloads it itself, which would only verify
        # after the fact. If pre-downloading a local snapshot proves
        # inconvenient at bring-up, pinning HF's own commit sha (instead of
        # a content sha256) is the other option to evaluate.
        verify_checkpoint_sha256(self.config.checkpoint_id, self.config.checkpoint_sha256)
        # The model's OTHER inputs. WorldPipeline conditions on the WorldClim
        # bio rasters + data/global/etopo_10m.tif (via synthetic_map.
        # _compute_map_stats), so this gate is exactly as load-bearing as the
        # checkpoint one above: without it, a box with a different ETOPO
        # build generates different terrain under the same provider_id.
        #
        # DEFAULT_CONDITIONING_ROOT is passed EXPLICITLY (an argument beats
        # TERRAIN_CONDITIONING_ROOT) on purpose: upstream's
        # synthetic_map._compute_map_stats opens the relative path
        # data/global/... from the process CWD and knows nothing about our
        # env var. Honouring the override here would let the gate verify a
        # directory the model never opens -- passing against canonical data
        # while generating from a different local copy, which is exactly the
        # divergence this check exists to catch. The env var stays useful
        # for computing a digest off-box; it must not move the gate.
        verify_conditioning_digest(self.config, root=DEFAULT_CONDITIONING_ROOT)

        device = "cuda" if torch.cuda.is_available() else "cpu"
        # World-shape kwargs are forwarded to WorldPipeline.__init__ by
        # from_pretrained. This is the half the old `sampler` field never had:
        # the values in the identity are now the values the model receives.
        pipeline = WorldPipeline.from_pretrained(
            self.config.checkpoint_id, **self.config.world_shape.as_pipeline_kwargs()
        )
        pipeline.to(device)
        # 'direct' (in-memory LRU) is WorldPipeline.bind's own default
        # caching_strategy; no hdf5_file needed for a single serverless
        # worker process. See world_pipeline.py's `bind()`.
        pipeline.bind()
        self._pipeline = pipeline
        return pipeline

    def _pipeline_for_seed(self, seed: int):
        pipeline = self._pipeline if self._pipeline is not None else self._load_pipeline()
        if self._bound_seed != seed:
            # change_seed() rebuilds the pipeline's internal tile
            # hierarchy -- expensive, but expected to be rare: one server
            # process serves one world seed for the whole session in
            # practice (per docs/diffusion-bringup.md's cost model).
            pipeline.change_seed(seed)
            self._bound_seed = seed
        return pipeline

    def _get_native(self, pipeline, i1: int, j1: int, i2: int, j2: int, scale: int):
        """Fetch one tile's rasters at the model's NATIVE 30 m resolution.

        **The scale>1 branch that used to live here is deleted, deliberately.**
        It transcribed ``terrain_diffusion.inference.api._get_terrain``'s
        bilinear upsample (``mode="bilinear"``, ``align_corners=False``, a
        1-native-pixel pad, a ceil-div and matching crop offsets) and produced
        a scale-8 tile carrying **exactly zero information the scale-1 tile did
        not already have** -- the learned cascade ends at 30 m
        (``amplifier.cpp:262-266``), so upsampling 8x is an interpolator, not a
        model. docs/terrain-amplification-plan.md: "The 3.75 m/px tier already
        exists in the format and carries zero information today. That is the
        slot this fills."

        The slot is now filled by the geomorphic bake
        (``terrain_service.bake.pipeline``), which writes a real scale-16 fine
        tier at 1.875 m/px. Removing the fake path is what makes the fine tier
        unambiguous: a cached sub-30 m tile is now either baked or absent, and
        can never be a bilinear stand-in that looks like data.

        Anything above scale 1 is therefore refused here rather than
        interpolated. See ``pregen.py --mode bake``.
        """
        if scale != 1:
            raise ValueError(
                f"terrain-diffusion generates at 30 m/px only (scale=1), got "
                f"scale={scale}. The sub-30 m tier is BAKED, not upsampled: run "
                "`python -m terrain_service.pregen --mode bake` "
                "(terrain_service.bake.pipeline). The old bilinear scale-8 path "
                "was deleted because it carried no information the scale-1 tile "
                "did not already have."
            )
        out = pipeline.get(i1, j1, i2, j2, with_climate=True)
        return out["elev"], out["climate"]

    def coarse_elevation_m(
        self, seed: int, ci0: int, ci1: int, cj0: int, cj1: int
    ) -> np.ndarray:
        """The model's COARSE-STAGE elevation over a cell window, in metres.

        Rows ``[ci0, ci1)`` and columns ``[cj0, cj1)`` of the coarse map. Per
        ``tools/world_map.py::check_axis_mapping`` (r = +0.999 against the
        cached tiles, -0.795 transposed) ``ci`` is the tile-Y axis and ``cj``
        is tile-X, the same orientation ``generate_rasters`` uses.

        WHY THIS EXISTS. The hydrology pyramid's top level receives no inflow
        at its own edges, so a catchment larger than its span is truncated
        (``bake.pipeline.HYDROLOGY_RESIDUALS`` #2). The coarse map is the
        cheapest possible parent: one cell is ``FINE_PX_PER_COARSE_CELL`` * 30
        m = 7.68 km, a 512^2 window therefore spans 3,932 km, and it costs
        coarse-stage inference ONLY -- it touches neither the latent stage nor
        the decoder, which are what make a tile expensive.

        The access idiom is the explorer's own
        (``terrain_diffusion/inference/explorer/server.py::_coarse_channel``):
        divide by the accumulator plane, take channel 0, then undo the
        signed-sqrt encoding. Transcribed rather than imported because the
        explorer is a Flask app that pulls in matplotlib.

        Returns float32 so it can go straight to ``build_model_superblock``.
        """
        pipeline = self._pipeline_for_seed(seed)
        import torch

        # The coarse stage is DETERMINISTIC given the world seed -- it is
        # hashed per tile inside the pipeline's tile store, exactly like tile
        # generation. No torch.manual_seed here on purpose: unlike
        # generate_rasters there is no (x, y, scale) to derive one from, and
        # pinning the global RNG from the bare world seed would make the answer
        # depend on the ORDER windows were requested in. Determinism is
        # asserted by test rather than assumed -- see the scope doc's test 1.
        with torch.no_grad():
            c = pipeline.coarse[:, ci0:ci1, cj0:cj1]
            norm = (c[:-1] / (c[-1:] + 1e-8))[0]
            elev = torch.sign(norm) * torch.square(norm)
            out = elev.detach().to("cpu").to(torch.float32).numpy()
        if out.shape != (ci1 - ci0, cj1 - cj0):
            raise RuntimeError(
                f"coarse window came back {out.shape}, asked for "
                f"{(ci1 - ci0, cj1 - cj0)} -- world.coarse is not being indexed "
                "in world coordinates the way this assumes"
            )
        return np.ascontiguousarray(out, dtype=np.float32)

    def coarse_cell_fine_px(self, seed: int) -> int:
        """Fine (30 m) pixels per coarse cell, ASKED OF THE LIVE PIPELINE.

        ``_compute_climate`` indexes the coarse map at ``i // (32 * scale)``
        with ``scale = latent_compression``, so the ratio is
        ``32 * latent_compression`` -- 256 at the shipped ``latent_compression
        = 8``, which is where "one coarse cell is 7.68 km" comes from.

        Derived rather than hard-coded because getting it wrong is silent: a
        window built at the wrong ratio is georeferenced to the wrong ground
        and injects a real river into the wrong place, with nothing in the
        output to say so. ``FINE_PX_PER_COARSE_CELL`` is the value every other
        tool in this repo assumes, and a checkpoint that disagreed with it
        would break those tools too, so this refuses rather than adapts.
        """
        pipeline = self._pipeline_for_seed(seed)
        ratio = int(32 * int(pipeline.latent_compression))
        if ratio != FINE_PX_PER_COARSE_CELL:
            raise RuntimeError(
                f"this checkpoint puts {ratio} fine px in a coarse cell, not "
                f"{FINE_PX_PER_COARSE_CELL}. tools/world_map.py, the 7.68 km "
                "coarse cell in docs, and the flow pyramid's model window all "
                "assume the latter; reconcile them deliberately rather than "
                "letting one tool silently use a different world grid."
            )
        return ratio

    def generate_rasters(self, seed: int, x: int, y: int, scale: int) -> dict[str, np.ndarray]:
        # _pipeline_for_seed -> _load_pipeline does the guarded `import
        # torch` (clear RuntimeError if missing) BEFORE anything below
        # assumes torch is importable.
        pipeline = self._pipeline_for_seed(seed)
        import torch

        # Defense-in-depth determinism, layered on top of WorldPipeline's
        # own internal per-tile seeding (see class docstring): pin torch's
        # global RNG from a documented hash of (seed, x, y, scale)
        # immediately before inference, in case the sampler's forward pass
        # consumes any randomness NOT already covered by the tile store's
        # own deterministic per-tile hashing. Per doctrine this targets
        # same-machine reruns only -- see derive_tile_seed's docstring.
        tile_seed = derive_tile_seed(seed, x, y, scale)
        torch.manual_seed(tile_seed & 0xFFFFFFFFFFFFFFFF)

        # CONFIRMED 2026-07-25 (was an open ASSUMPTION since bring-up): tile
        # (x, y) covers pixels [x*TILE_SIZE,(x+1)*TILE_SIZE) per tile_codec.py,
        # with x=column (j) and y=row (i). world_pipeline.py names its own
        # internal tile-hash args (ty, tx) row-then-col, which is what this
        # mapping mirrors, but WorldPipeline.get()'s (i1, j1, i2, j2) axis
        # order could not be settled from source alone.
        #
        # It did not need a GPU after all. The COARSE model underlies both the
        # generated tiles and the explorer's coarse map, so the mean elevation
        # of each cached .vxtl must correlate with the coarse cells at its own
        # footprint -- under the correct orientation only. Measured over the 25
        # tiles of seed 20260719 by `tools/world_map.py --verify-axes`:
        #
        #     ci<-y, cj<-x  (this mapping): r = +0.999
        #     ci<-x, cj<-y  (transposed):   r = -0.795
        #
        # Emphatic, not a near-tie, so the check is conclusive rather than
        # suggestive. Re-run that command on a new seed if this line ever
        # changes; note GENERATION_ALGORITHM_VERSION must be bumped if it does.
        i1, j1 = y * TILE_SIZE, x * TILE_SIZE
        i2, j2 = i1 + TILE_SIZE, j1 + TILE_SIZE

        elev, climate = self._get_native(pipeline, i1, j1, i2, j2, scale)

        elev_np = elev.detach().to("cpu").to(torch.float32).numpy()
        climate_np = climate.detach().to("cpu").to(torch.float32).numpy()

        mapping = self.config.channel_mapping
        raster: dict[str, np.ndarray] = {
            mapping["elevation"]: np.ascontiguousarray(elev_np),
        }
        for i, name in enumerate(_CLIMATE_ORDER):
            raster[mapping[name]] = np.ascontiguousarray(climate_np[i])
        return raster


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
        self,
        config: DiffusionConfig | None = None,
        dry_run: bool = False,
        model_backend: ModelBackend | None = None,
    ) -> None:
        self.config = config or DiffusionConfig()
        self.dry_run = dry_run
        #: Optional injected model backend (see ModelBackend protocol) --
        #: how this is unit-tested without a GPU (a fake backend stands in
        #: for TerrainDiffusionBackend). None (the default) keeps prior
        #: behavior/signature compatibility and means "lazily construct the
        #: real TerrainDiffusionBackend on first non-dry-run call".
        self.model_backend = model_backend
        self._real_backend: TerrainDiffusionBackend | None = None
        # provider_id is an attribute (not a property) per the TileProvider
        # protocol; dry-run runs are tagged so they can never collide with
        # real generated tiles in a shared cache.
        base_id = self.config.provider_id()
        if dry_run:
            # Tag dry-run output so it can never collide with real generated
            # tiles -- and tag it with the STAND-IN's own version, because in
            # dry-run mode SyntheticProvider, not the checkpoint, is what
            # decides the bytes. Without this, bumping "synthetic-v1" (the
            # exact thing that string exists for) would leave every dry-run
            # tile cached under an unchanged id.
            from .synthetic import SyntheticProvider

            self.provider_id = f"{base_id}-dryrun-{SyntheticProvider.provider_id}"
        else:
            self.provider_id = base_id
        # The bake-derived namespace, derived from the id ABOVE rather than
        # from config.provider_id(), so a dry-run's fine tier inherits the
        # -dryrun- tag instead of landing in the real namespace.
        self.fine_provider_id = fine_id_for(self.provider_id)

    def generate(self, seed: int, x: int, y: int, scale: int) -> Tile:
        if scale != self.config.scale:
            raise ValueError(
                f"DiffusionProvider configured for scale={self.config.scale}, "
                f"got scale={scale} — bring up a separate DiffusionConfig "
                "(and provider_id) per scale"
            )
        raster = self._call_model(seed, x, y, scale)
        # Before validation, not instead of it: this only pulls in excursions
        # small enough to be model noise about a physical floor (see
        # clamp_to_physical_range). Anything larger still reaches
        # validate_model_output and still fails.
        clamped = clamp_to_physical_range(raster, self.config.channel_mapping)
        if clamped:
            self._clamped_cells = clamped
        validate_model_output(raster, self.config.channel_mapping)
        return adapt_raster_to_tile(raster, self.config, seed, x, y, scale)

    # -- the coarse stage, for the hydrology pyramid's top level -------------
    #
    # Deliberately NOT part of the TileProvider protocol. A provider that
    # cannot serve its own coarse map (synthetic, dry-run) simply does not have
    # these, and ``pregen`` duck-types for them and says so; a protocol method
    # would have forced every provider to grow a stub that raises, which is the
    # same absence spelled at more length.

    def coarse_elevation_m(
        self, seed: int, ci0: int, ci1: int, cj0: int, cj1: int
    ) -> np.ndarray:
        """Coarse-stage elevation over a cell window, metres. See the backend's.

        Refused in dry-run mode: the synthetic stand-in has no coarse STAGE,
        only per-tile rasters, so anything returned here would be a fabricated
        parent for the flow pyramid -- rivers invented out of nothing, in a
        world whose id says it is only standing in for the plumbing.
        """
        return self._coarse_backend().coarse_elevation_m(seed, ci0, ci1, cj0, cj1)

    def coarse_cell_fine_px(self, seed: int) -> int:
        """Fine pixels per coarse cell, asked of the live pipeline."""
        return self._coarse_backend().coarse_cell_fine_px(seed)

    def _coarse_backend(self):
        if self.dry_run:
            raise NotImplementedError(
                "dry-run mode has no coarse stage to read: its rasters come "
                "from SyntheticProvider one tile at a time. A model-backed "
                "flow parent needs the real checkpoint."
            )
        backend = self.model_backend
        if backend is None:
            if self._real_backend is None:
                self._real_backend = TerrainDiffusionBackend(self.config)
            backend = self._real_backend
        if not hasattr(backend, "coarse_elevation_m"):
            raise NotImplementedError(
                f"{type(backend).__name__} does not expose the coarse stage "
                "(coarse_elevation_m); only TerrainDiffusionBackend does."
            )
        return backend

    def _call_model(self, seed: int, x: int, y: int, scale: int) -> dict[str, np.ndarray]:
        """Get raw (unquantized) model rasters for one tile.

        Precedence (dry_run wins): dry_run=True ALWAYS uses the synthetic
        stand-in below, even if a model_backend was injected -- dry_run
        exists specifically to exercise the surrounding plumbing (config ->
        adapter -> validate -> encode) without any model, real or fake,
        being invoked, so it must behave identically regardless of what a
        caller happens to have injected. Otherwise, an injected
        model_backend (see ModelBackend protocol) is used if present -- this
        is how bring-up scaffolding is unit-tested without a GPU (a fake
        backend stands in for TerrainDiffusionBackend). With neither, the
        real ``TerrainDiffusionBackend`` is lazily constructed exactly once
        (needs a CUDA machine; see terrain-service/docs/diffusion-bringup.md)
        and reused for subsequent calls.
        """
        if self.dry_run:
            return self._synthetic_stand_in(seed, x, y, scale)
        backend = self.model_backend
        if backend is None:
            if self._real_backend is None:
                self._real_backend = TerrainDiffusionBackend(self.config)
            backend = self._real_backend
        return backend.generate_rasters(seed, x, y, scale)

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
