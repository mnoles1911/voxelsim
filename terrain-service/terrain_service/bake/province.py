"""Landform provinces -- per-cell parameter FIELDS for the fine bake (Tier 1).

See ``docs/landform-provinces-plan.md``. The problem this exists to fix, stated
as a player would state it: the fine tier applies ONE GLOBAL CONSTANT SET
everywhere, so similar relief plus similar climate produces siblings.

THE LOAD-BEARING PRINCIPLE, and the reason this module is small: **a province
is derived from the terrain the model already produced, never hashed
independently of it.** A hashed province field that says "glacial mountains"
where the model produced a coastal plain is incoherent, and players forgive
sameness far more readily than they forgive nonsense.

WHAT THIS IS NOT. It is not a per-tile constant-set lookup. That is the obvious
design and it is the wrong one: two adjacent tiles baked with different
constants disagree along their shared edge. Here the province is a per-cell
FIELD and each province-varying constant becomes a per-cell PARAMETER field, so
blending between provinces is a smoothstep on the field rather than a special
case in the code.

WHERE IT LIVES, AND WHY THAT MATTERS TWICE. Every field in this module is
computed on the PADDED COARSE domain (576^2 at production, 30 m/px) and is
consumed by indexing at ``//scale`` at the point of use -- never
``np.repeat``-ed to the fine grid. That is the pattern
``profile_incision(regional_scale=...)`` already established, and it is worth
340 MB per field inside the bake's peak stage. It is also what makes the
apron argument work: every input is a pure function of the coarse rasters the
960 m apron already covers, and the only non-pointwise step (the landform-scale
smooth, ``province_smooth_m``) has an influence radius bounded well under the
apron, so two neighbouring tiles compute identical values throughout their
overlap.

WHAT THAT DOES *NOT* BUY. It does not restore a seam GUARANTEE, because there
is not one to restore: ``pipeline.APRON_BLIND_SPOT`` measured 1.05% of the
shipped interior moving past the 100 mm wire LSB (by up to 78.79 m) when the
apron was widened, with the domain border's influence reaching 3.8 km inward --
four apron widths -- because the depression fill is unbounded and a truncated
domain INVENTS AN OUTLET. Province fields do not make that worse and do not
make it better. The honest claim is "no new influence radius", not "seam-safe".

THE CLIMATE CAVEAT. Climate arrives at 30 m/px and uint8-quantised (see
``CLIMATE_RANGES``: precipitation's LSB is 47 mm/yr). Used naively it prints
30 m blocks into erosion intensity. Every climate discriminant here is
therefore smoothed to landform scale FIRST, on the padded domain, before it
reaches a threshold.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Mapping

import numpy as np

__all__ = [
    "CLIMATE_ORDER",
    "CLIMATE_RANGES",
    "PROVINCES",
    "PROVINCE_MULTIPLIERS",
    "ProvinceMultipliers",
    "ProvinceFields",
    "dequantize_climate",
    "province_weights",
    "province_fields",
    "identity_payload",
    "smoothstep",
    "box_smooth",
]


#: Plane order inside the tile's ``(4, 512, 512)`` uint8 climate array. This is
#: ``tile_codec``'s packing order and ``diffusion._CLIMATE_ORDER``'s; it is wire
#: format, not a local convention.
CLIMATE_ORDER: tuple[str, ...] = (
    "temperature",
    "seasonality",
    "precipitation",
    "precip_variability",
)

#: PHYSICAL range each uint8 plane was quantised against, i.e. the inverse of
#: ``diffusion.adapt_raster_to_tile``'s ``round((raw - min) / (max - min) * 255)``.
#:
#: This is the THIRD copy of the climate wire format (diffusion.py's
#: ``EXPECTED_CHANNELS`` and ``voxel-core/include/voxelcore/climate.h`` are the
#: other two), and it is duplicated here deliberately rather than imported:
#: ``terrain_service.bake`` must not depend on ``terrain_service.providers``,
#: which pulls in the model stack. ``tests/test_climate_contract.py`` asserts
#: all three agree, so a divergence fails there by name instead of silently
#: shifting every province boundary.
#:
#: Note the LSBs these imply -- they are why nothing here reads climate without
#: smoothing it first: temperature 0.314 C, seasonality 11.8, precipitation
#: 47.1 mm/yr, precip_variability 0.784.
CLIMATE_RANGES: Mapping[str, tuple[float, float]] = {
    "temperature": (-40.0, 40.0),
    "seasonality": (0.0, 3000.0),
    "precipitation": (0.0, 12000.0),
    "precip_variability": (0.0, 200.0),
}


@dataclass(frozen=True)
class ProvinceMultipliers:
    """One province's factors on today's GLOBAL constants.

    Multipliers rather than absolute values, so ``FLUVIAL`` is all ones and
    reproduces the calibrated pipeline exactly -- which is what
    ``docs/landform-provinces-plan.md`` means by "FLUVIAL: today's pipeline,
    unchanged". Every other province is then read as a departure from a
    calibrated baseline rather than as an independent tuning surface, which is
    the plan's risk #3 ("six provinces x six constants is 36 numbers").
    """

    #: ``profile_K_dt``: fluvial incision rate.
    k_dt: float = 1.0
    #: ``channel_init_area_m2``: channel-initiation area. HIGHER = coarser
    #: drainage network (fewer, more widely spaced channels).
    a_crit: float = 1.0
    #: ``stream_m``: the area exponent, hence concavity theta = m/n.
    stream_m: float = 1.0
    #: ``channel_init_q``: sharpness of the hillslope-to-channel transition.
    gate_q: float = 1.0
    #: ``meso_amp15_m`` / ``meso_amp11_m`` together (they are one band).
    meso_amp: float = 1.0


#: Blend order. FLUVIAL is index 0 and is the baseline every other province is
#: a departure from.
PROVINCES: tuple[str, ...] = ("fluvial", "glacial", "arid", "lowland")

#: THE TUNING SURFACE, all of it, in one table.
#:
#: These are first-cut values chosen to match the *signatures* named in the
#: plan's taxonomy table, and they are the part of this change least supported
#: by measurement -- the plan itself asks that every province constant be
#: derived from a real-world measurement and recorded with its source, and that
#: work has not been done. What IS measured is the mechanism (the fields reach
#: the solve and separate the classes; see ``tests/test_province.py``), not the
#: calibration. Read them as a starting point for the re-measure step, not as
#: an answer.
PROVINCE_MULTIPLIERS: Mapping[str, ProvinceMultipliers] = {
    # Today's pipeline, by construction.
    "fluvial": ProvinceMultipliers(),
    # GLACIAL -- "U-valleys, cirques, overdeepened basins". Tier 1 explicitly
    # CANNOT make a U-valley: that needs the Tier 3 widening kernel, because no
    # amount of constant-tuning on a stream-power law produces a U
    # cross-section. What Tier 1 can honestly do is stop pretending water did
    # the work: ice erased the fine dendritic network (a_crit up, so the
    # hillslope texture is smooth between widely spaced trunks), incision by
    # running water is a minority process (k_dt down), the long profile is
    # much less area-dependent than a fluvial one (stream_m down -> lower
    # concavity, which is the one glacial/fluvial statistic with published
    # separation), and the rock morphology is rougher at meso scale --
    # headwalls, benches, roches moutonnees (meso_amp up).
    "glacial": ProvinceMultipliers(
        k_dt=0.60, a_crit=3.0, stream_m=0.75, gate_q=1.0, meso_amp=1.40
    ),
    # ARID -- "pediments, sharp divides, badlands, internal drainage".
    # High incision when it does rain plus near-zero creep gives sharp,
    # un-rounded divides: k_dt up and gate_q up (a sharper hillslope-to-channel
    # transition IS a sharp divide). Coarse drainage: a_crit up hardest of the
    # four, because a dry landscape concentrates runoff into few large washes.
    # CAVEAT, and it is a large one: the shipped conditioning classifies to
    # DESERT 1.84% and SAVANNA 0.00% (plan, "What provinces do not solve"), so
    # this province is currently almost unreachable and therefore untuned and
    # unjudged. It is wired, not calibrated.
    "arid": ProvinceMultipliers(
        k_dt=1.60, a_crit=4.0, stream_m=1.15, gate_q=1.30, meso_amp=1.30
    ),
    # LOWLAND -- "subdued, alluvial, wide floodplains". Heavy deposition and
    # low incision: k_dt is the lowest of the four. Wide floodplains mean few
    # channels per unit area (a_crit up, though less than arid -- a humid
    # lowland still has a dendritic network, it is just widely spaced), a very
    # concave alluvial profile (stream_m up slightly), and a meso band at half
    # amplitude because a floodplain is the one place the 11-15 m band should
    # be quietest. Note the existing meso slope gate already zeroes this band
    # on gentle ground, so meso_amp here mostly affects the lowland's steeper
    # margins.
    "lowland": ProvinceMultipliers(
        k_dt=0.45, a_crit=2.5, stream_m=1.10, gate_q=1.0, meso_amp=0.50
    ),
}


def identity_payload() -> dict:
    """The part of the province definition that decides baked bytes.

    Folded into ``pipeline.bake_identity_payload`` so that retuning the table
    above rolls ``fine_provider_id`` -- the same obligation every bake constant
    already carries. The thresholds are NOT here: they live on
    ``BakeConstants`` and ride in ``consts.as_payload()``.
    """
    return {
        "provinces": list(PROVINCES),
        "multipliers": {
            name: [
                PROVINCE_MULTIPLIERS[name].k_dt,
                PROVINCE_MULTIPLIERS[name].a_crit,
                PROVINCE_MULTIPLIERS[name].stream_m,
                PROVINCE_MULTIPLIERS[name].gate_q,
                PROVINCE_MULTIPLIERS[name].meso_amp,
            ]
            for name in PROVINCES
        },
        "climate_ranges": {k: list(v) for k, v in CLIMATE_RANGES.items()},
    }


# ---------------------------------------------------------------------------
# Primitives.
# ---------------------------------------------------------------------------


def smoothstep(x, lo: float, hi: float) -> np.ndarray:
    """C1 ramp: 0 at or below ``lo``, 1 at or above ``hi``.

    The plan's "blending between provinces is then free: a smoothstep on the
    field, not a special case in the code". C1 rather than linear because a
    kink in a parameter field is a kink in the erosion rate, and the worldgen
    v20 banding investigation exists because of exactly that class of artifact.
    """
    if not hi > lo:
        raise ValueError(f"smoothstep needs lo < hi, got {lo}, {hi}")
    t = np.clip((np.asarray(x, dtype=np.float32) - np.float32(lo))
                / np.float32(hi - lo), 0.0, 1.0)
    return (t * t * (3.0 - 2.0 * t)).astype(np.float32, copy=False)


def _box1d(a: np.ndarray, half: int, axis: int) -> np.ndarray:
    """One separable box pass of half-width ``half``, edge-clamped.

    Deliberately cumsum rather than ``scipy.ndimage.uniform_filter``:
    ``pipeline.py`` may not import scipy (it is a bake-pod dependency, and CI
    does not install it), and this runs on a 576^2 coarse array where the
    difference is microseconds.
    """
    if half <= 0:
        return a
    a = np.moveaxis(a, axis, -1)
    n = a.shape[-1]
    # Edge-clamped padding, so the filter is "nearest" like every other edge
    # convention in the bake.
    pad = np.concatenate(
        [np.repeat(a[..., :1], half, axis=-1), a,
         np.repeat(a[..., -1:], half, axis=-1)], axis=-1)
    c = np.cumsum(pad.astype(np.float64), axis=-1)
    c = np.concatenate([np.zeros(c.shape[:-1] + (1,), np.float64), c], axis=-1)
    win = 2 * half + 1
    out = (c[..., win:win + n] - c[..., :n]) / float(win)
    return np.moveaxis(out.astype(np.float32), -1, axis)


def box_smooth(a: np.ndarray, half: int) -> np.ndarray:
    """TWO separable box passes -- a triangle kernel of total radius ``2*half``.

    One box pass has a sinc transfer function that leaves ringing at the block
    scale, which on a uint8-quantised 30 m climate plane is precisely the
    artifact ``province_smooth_m`` exists to remove. Two passes are still four
    O(n) sweeps.

    The INFLUENCE RADIUS is ``2 * half`` cells and that number is the apron
    obligation: the caller must keep it under the apron so a tile's interior
    never reads a cell its neighbour's padded domain lacks.
    """
    if half <= 0:
        return np.asarray(a, dtype=np.float32)
    out = np.asarray(a, dtype=np.float32)
    for _ in range(2):
        out = _box1d(_box1d(out, half, 0), half, 1)
    return out


def dequantize_climate(planes: np.ndarray) -> dict[str, np.ndarray]:
    """uint8 climate planes -> physical units, per ``CLIMATE_RANGES``.

    ``planes`` is ``(4, H, W)`` uint8 in ``CLIMATE_ORDER``. Exactly inverts
    ``diffusion.adapt_raster_to_tile``'s quantisation up to its rounding, which
    is 1/255 of the range and is the reason every consumer smooths first.
    """
    p = np.asarray(planes)
    if p.ndim != 3 or p.shape[0] != len(CLIMATE_ORDER):
        raise ValueError(
            f"climate planes must be ({len(CLIMATE_ORDER)}, H, W), got {p.shape}")
    out = {}
    for i, name in enumerate(CLIMATE_ORDER):
        lo, hi = CLIMATE_RANGES[name]
        out[name] = (np.float32(lo)
                     + p[i].astype(np.float32) * np.float32((hi - lo) / 255.0))
    return out


# ---------------------------------------------------------------------------
# The province field itself.
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class ProvinceFields:
    """Coarse-resolution province weights and the parameter fields from them.

    Every array is ``(padded_coarse_px, padded_coarse_px)`` float32 and is
    consumed by ``//scale`` indexing at the fine grid. ``weights`` sums to 1.0
    per cell (a partition, per the plan's taxonomy), so a parameter field is a
    weighted blend and never a branch.
    """

    weights: dict[str, np.ndarray]
    k_dt: np.ndarray
    a_crit_m2: np.ndarray
    stream_m: np.ndarray
    gate_q: np.ndarray
    meso_amp15_m: np.ndarray
    meso_amp11_m: np.ndarray
    #: The smoothed discriminants, for probes and tests. Not consumed by the bake.
    relief: np.ndarray
    elev_m: np.ndarray
    temp_c: "np.ndarray | None"
    precip_mm: "np.ndarray | None"


def province_weights(
    coarse_elev_m: np.ndarray,
    climate: "np.ndarray | None",
    *,
    coarse_pixel_m: float,
    smooth_m: float,
    relief_lo: float,
    relief_hi: float,
    cold_c: float,
    temperate_c: float,
    arid_mm: float,
    humid_mm: float,
    lowland_elev_lo_m: float,
    lowland_elev_hi_m: float,
    strength: float = 1.0,
    max_half: "int | None" = None,
) -> tuple[dict[str, np.ndarray], dict[str, np.ndarray]]:
    """Soft province membership on the padded coarse grid.

    Returns ``(weights, discriminants)``. Weights sum to 1.0 per cell.

    THE RELIEF DISCRIMINANT IS FREE. It is ``|grad(coarse elevation)|`` at the
    coarse pitch, i.e. the same 30 m-scale slope
    ``pipeline`` already builds as the "regional energy factor" that multiplies
    ``kfac`` -- world-anchored by construction, because it is a pure function of
    the carrier rather than of independently hashed noise. The only thing added
    here is the landform-scale smooth.

    ``max_half`` CLAMPS the smoothing half-width, and it is the apron
    obligation made mechanical. The smooth is the only non-pointwise step
    province adds; its influence radius is ``2 * half`` coarse cells, and the
    caller passes ``apron_coarse_px // 4`` so that radius is at most HALF the
    apron on any geometry. Clamping rather than raising is deliberate: the
    small test geometries have a 4-cell apron, and a constant tuned for
    production must not make them unbakeable. The clamp cannot silently change
    production bytes under you, because the geometry it depends on is already
    hashed into the bake identity.

    ``climate`` is the ``(4, H, W)`` uint8 plane stack, or None. With None the
    partition degrades to relief and elevation alone: GLACIAL and ARID both
    collapse to zero (neither can be inferred without temperature or
    precipitation) and the world is FLUVIAL/LOWLAND. That is the deliberate
    behaviour for a caller with no climate rather than an error, because every
    existing test double and ``tools/bake_real_tile.py``'s v1-tile path supply
    elevation only.
    """
    z = np.asarray(coarse_elev_m, dtype=np.float32)
    if z.ndim != 2:
        raise ValueError(f"coarse_elev_m must be 2-D, got {z.shape}")
    half = max(0, int(round(float(smooth_m) / float(coarse_pixel_m) / 2.0)))
    if max_half is not None:
        half = min(half, max(0, int(max_half)))

    gy, gx = np.gradient(z.astype(np.float64), float(coarse_pixel_m))
    relief = box_smooth(np.hypot(gx, gy).astype(np.float32), half)
    del gy, gx
    elev = box_smooth(z, half)

    steep = smoothstep(relief, relief_lo, relief_hi)
    low_elev = 1.0 - smoothstep(elev, lowland_elev_lo_m, lowland_elev_hi_m)

    temp_c = precip_mm = None
    if climate is not None:
        phys = dequantize_climate(climate)
        temp_c = box_smooth(phys["temperature"], half)
        precip_mm = box_smooth(phys["precipitation"], half)
        cold = 1.0 - smoothstep(temp_c, cold_c, temperate_c)
        dry = 1.0 - smoothstep(precip_mm, arid_mm, humid_mm)
    else:
        cold = np.zeros_like(steep)
        dry = np.zeros_like(steep)

    # Raw memberships. Each is a product of independent soft conditions, so
    # each is already in [0, 1].
    raw = {
        # cold AND high relief. A cold plain is not glaciated terrain in any
        # way the fine tier can express.
        "glacial": cold * steep,
        # dry, and NOT cold -- a cold desert reads as periglacial/glacial here,
        # and letting both fire would double-count the same ground.
        "arid": dry * (1.0 - cold),
        # gentle AND low AND not dry. The "not dry" keeps an arid basin from
        # also claiming heavy alluvial deposition.
        "lowland": (1.0 - steep) * low_elev * (1.0 - dry),
    }
    s = float(np.clip(strength, 0.0, None))
    for name in raw:
        raw[name] = (raw[name] * np.float32(s)).astype(np.float32)

    # FLUVIAL IS THE RESIDUAL, and it has to be, not a constant baseline. With
    # a baseline of 1.0 the normalisation caps every other province at 0.5 --
    # a fully cold, fully steep cell would come out half FLUVIAL, so GLACIAL's
    # multipliers could never apply at more than half strength and the plan's
    # "a province whose output cannot be distinguished from FLUVIAL should be
    # cut" would be self-fulfilling. (Measured while writing this: a maximally
    # glacial synthetic domain scored exactly 0.5/0.5.)
    #
    # The residual is the PRODUCT of the complements rather than
    # ``1 - sum(raw)``: that form stays in [0, 1] with no clamp, is C1
    # everywhere -- a ``where(sum > 1, ...)`` renormalisation has a kink on the
    # sum == 1 contour, and a kink in a parameter field is a kink in the
    # erosion rate -- and it reads correctly, "fluvial is what is left when
    # nothing else claims the ground".
    raw["fluvial"] = np.ones_like(steep)
    for name in PROVINCES[1:]:
        raw["fluvial"] = raw["fluvial"] * (1.0 - raw[name])
    total = sum(raw[name] for name in PROVINCES)
    weights = {name: (raw[name] / total).astype(np.float32) for name in PROVINCES}
    disc = {"relief": relief, "elev_m": elev,
            "temp_c": temp_c, "precip_mm": precip_mm}
    return weights, disc


def _blend(weights: dict[str, np.ndarray], attr: str, base: float) -> np.ndarray:
    """Weighted GEOMETRIC blend of the per-province multipliers, times ``base``.

    Geometric (a weighted mean in log space) rather than arithmetic because
    these are RATE constants: a 3x province and a 1/3x province meeting
    half-and-half should read 1x, not 1.67x. It also cannot produce a
    non-positive value from positive multipliers, which is what keeps the
    ``a_crit > 0`` and ``K_dt >= 0`` invariants true by construction rather
    than by clipping.
    """
    acc = np.zeros(next(iter(weights.values())).shape, dtype=np.float32)
    for name in PROVINCES:
        m = float(getattr(PROVINCE_MULTIPLIERS[name], attr))
        if m <= 0.0:
            raise ValueError(f"province {name}.{attr} must be positive, got {m}")
        acc += weights[name] * np.float32(np.log(m))
    return (np.float32(base) * np.exp(acc)).astype(np.float32)


def province_fields(
    coarse_elev_m: np.ndarray,
    climate: "np.ndarray | None",
    *,
    coarse_pixel_m: float,
    consts,
    max_half: "int | None" = None,
) -> ProvinceFields:
    """Build every Tier 1 parameter field for one padded coarse domain.

    ``consts`` is a ``pipeline.BakeConstants``; the province thresholds and the
    base constants both come from it, so the whole thing rolls the bake
    identity through ``consts.as_payload()``. ``max_half`` is the apron clamp --
    see ``province_weights``.
    """
    weights, disc = province_weights(
        coarse_elev_m,
        climate,
        coarse_pixel_m=coarse_pixel_m,
        smooth_m=consts.province_smooth_m,
        relief_lo=consts.province_relief_lo,
        relief_hi=consts.province_relief_hi,
        cold_c=consts.province_cold_c,
        temperate_c=consts.province_temperate_c,
        arid_mm=consts.province_arid_mm,
        humid_mm=consts.province_humid_mm,
        lowland_elev_lo_m=consts.province_lowland_elev_lo_m,
        lowland_elev_hi_m=consts.province_lowland_elev_hi_m,
        strength=consts.province_strength,
        max_half=max_half,
    )
    return ProvinceFields(
        weights=weights,
        k_dt=_blend(weights, "k_dt", consts.profile_K_dt),
        a_crit_m2=_blend(weights, "a_crit", consts.channel_init_area_m2),
        stream_m=_blend(weights, "stream_m", consts.stream_m),
        gate_q=_blend(weights, "gate_q", consts.channel_init_q),
        meso_amp15_m=_blend(weights, "meso_amp", consts.meso_amp15_m),
        meso_amp11_m=_blend(weights, "meso_amp", consts.meso_amp11_m),
        relief=disc["relief"],
        elev_m=disc["elev_m"],
        temp_c=disc["temp_c"],
        precip_mm=disc["precip_mm"],
    )
